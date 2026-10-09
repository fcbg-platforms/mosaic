#include "video/video_manager.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

#include "trigger/trigger_manager.hpp"
#include "utils/logger.hpp"
#include "utils/ring_buffer.hpp"
#include "utils/timestamp.hpp"
#include "video/action_tick_log.hpp"
#include "video/camera_label.hpp"
#include "video/frame_shortfall.hpp"
#include "video/gige_action_command.hpp"
#include "video/video_encoder.hpp"
#include "video/video_grabber.hpp"

namespace mosaic {

// Ring buffer capacity: 2 seconds worth of frames at 60 fps = 120 slots.
static constexpr std::size_t k_ring_capacity = 128;

// Continuously fires GigE Vision Action Commands, one burst per frame, at a
// fixed period, for as long as this thread runs. Owns one ActionCommandSession
// for its whole lifetime, handed in already-constructed (see the constructor
// doc comment for why), so IssueActionCommand() is a lightweight per-tick
// call rather than paying transport-layer create/release cost on every
// single frame. The target list and period are fixed at construction —
// cameras don't get added/removed mid-recording, only at session boundaries,
// and a new ticker is always constructed fresh for each
// arm_and_fire_action_commands() call.
//
// Uses the same drift-free "nextTick += period" pacing idiom as
// VideoGrabber::run_stub_loop() (sleep in short increments, fire once
// now >= nextTick), rather than a single long sleep per tick, so
// requestInterruption() is noticed promptly rather than only after a full
// period elapses.
class ActionCommandTicker : public QThread {
   public:
    // session must already be valid (caller checks ActionCommandSession::
    // is_valid() and handles the failure case BEFORE constructing a ticker
    // at all — see arm_and_fire_action_commands()). Constructing the session
    // here in run() used to mean a transport-layer acquisition failure was
    // only ever a log line on a background thread, indistinguishable from
    // every Action1-armed camera simply working normally: no signal, no UI
    // change, nothing — reproducing the exact silent-black-camera failure
    // this whole redesign exists to eliminate. Taking an already-validated
    // session instead lets the caller fail loudly (emit camera_error) before
    // ever starting this thread.
    // grabbers is parallel to targets (same order, same length) — non-owning
    // pointers into VideoManager::d->units, which is guaranteed to outlive
    // this ticker (every path that could destroy a unit stops the ticker
    // first — see stop_action_ticker()'s call sites). Used only to read each
    // camera's own frames_grabbed() (already atomic-safe for cross-thread
    // reads) for the per-camera missed-trigger diagnostic in run() — never
    // written to from this thread.
    // tickLog, when given, receives one row per tick — only the recording
    // ticker gets one (see VideoManager::start()); preview ticks are not
    // worth a file. Owned here so it closes when this thread is done.
    ActionCommandTicker(std::unique_ptr<ActionCommandSession> session,
                        std::vector<ActionCommandTarget> targets,
                        std::vector<VideoGrabber*> grabbers, double periodMs,
                        std::unique_ptr<ActionTickLog> tickLog = nullptr)
        : m_session(std::move(session)),
          m_targets(std::move(targets)),
          m_grabbers(std::move(grabbers)),
          m_tickLog(std::move(tickLog)),
          m_period(std::chrono::duration<double, std::milli>(periodMs)) {
        // Baseline each camera's corrupted-frame counter, because it is the odd
        // one out: ticks_fired() starts at zero with this ticker and
        // frames_grabbed() is reset by start_grabbing(), but
        // incomplete_frames_total() is cumulative for the camera's whole open()
        // and deliberately never resets. Without this, a gap measured over
        // seconds would be compared against corruption accumulated across the
        // whole preview before it, every shortfall would classify as packet
        // loss, and "missing trigger broadcasts" would become unreachable.
        m_incompleteBaseline.reserve(m_grabbers.size());
        for (auto* g : m_grabbers) {
            m_incompleteBaseline.push_back(g ? g->incomplete_frames_total() : 0);
        }
        m_awayBaseline.assign(m_grabbers.size(), 0);
        m_seenReconnects.reserve(m_grabbers.size());
        for (auto* g : m_grabbers) {
            m_seenReconnects.push_back(g ? g->reconnects() : 0);
        }
    }

    // Total action-command ticks fired so far this ticker's lifetime.
    // Cross-thread-safe (atomic); read by VideoManager::stop_action_ticker()
    // right before this object is destroyed, since the running total would
    // otherwise be lost the instant that happens — see
    // VideoManager::action_ticks_fired()'s own doc comment.
    [[nodiscard]] int64_t ticks_fired() const {
        return m_ticksFired.load(std::memory_order_relaxed);
    }

   protected:
    void run() override {
        // ActionCommandSession::fire() already catches per-target
        // Pylon::GenericException internally, but this outer net catches
        // anything else — an uncaught exception escaping QThread::run() is
        // fatal to the whole process (std::terminate()), and this thread is
        // meant to run unattended for an entire recording session.
        try {
            auto nextTick      = SteadyClock::now();
            auto lastDiagTime  = nextTick;
            int64_t ticksFired = 0;
            while (!isInterruptionRequested()) {
                const auto now = SteadyClock::now();
                if (now < nextTick) {
                    QThread::msleep(1);
                    continue;
                }
                nextTick += std::chrono::duration_cast<SteadyClock::duration>(m_period);
                // Stamped immediately before the broadcast, on the frames' own
                // clock: the command fires immediately (no scheduled
                // ActionTime), so this is when the cameras were triggered.
                const int64_t tickElapsedNs = elapsed_ns();
                const int fired =
                    m_session->fire(m_targets, k_action_group_key, k_action_group_mask);
                if (m_tickLog) {
                    m_tickLog->append(ticksFired, tickElapsedNs, fired);
                }
                ++ticksFired;
                m_ticksFired.store(ticksFired, std::memory_order_relaxed);
                if (fired < static_cast<int>(m_targets.size())) {
                    log_warning(QString("[VideoManager] ActionCommandTicker: only %1/%2 action-"
                                        "command broadcasts succeeded this tick.")
                                    .arg(fired)
                                    .arg(m_targets.size()));
                }

                // Per-camera missed-trigger diagnostic, ~every 5s (same
                // cadence as VideoGrabber's own incomplete-frame warning).
                // fire() only reports whether the local UDP send succeeded —
                // IssueActionCommand() is fire-and-forget with no
                // acknowledgement, so a send that "succeeds" here says
                // nothing about whether the target camera actually received
                // it and captured a frame. Comparing ticksFired (how many
                // triggers this camera SHOULD have received) against its own
                // frames_grabbed() (how many it actually captured) surfaces
                // exactly that gap, per camera, live during the session —
                // distinct from the "incomplete frame" warning, which is
                // about a frame that started arriving and got corrupted, not
                // one that never arrived at all — and then says which of the
                // two this gap is, since they look identical from the frame
                // count alone and need opposite remedies.
                if (std::chrono::duration<double>(now - lastDiagTime).count() >= 5.0) {
                    lastDiagTime = now;
                    for (size_t i = 0; i < m_targets.size() && i < m_grabbers.size(); ++i) {
                        if (!m_grabbers[i]) {
                            continue;
                        }
                        // A camera that dropped out says so itself (see
                        // VideoGrabber::reconnect_after_loss()). Its frames
                        // are missing for that reason, not for a trigger or
                        // link fault, so it is left out while away, and once
                        // back it is judged from the moment it returned, or
                        // its time away would read as missed triggers for
                        // the rest of the session. Compared by reconnect
                        // count, not only the live flag: a short dropout can
                        // begin and end between two of these checks.
                        if (m_grabbers[i]->is_reconnecting()) {
                            continue;
                        }
                        const int64_t captured   = m_grabbers[i]->frames_grabbed();
                        const int64_t reconnects = m_grabbers[i]->reconnects();
                        if (reconnects != m_seenReconnects[i]) {
                            m_seenReconnects[i]     = reconnects;
                            m_awayBaseline[i]       = ticksFired - captured;
                            m_incompleteBaseline[i] = m_grabbers[i]->incomplete_frames_total();
                            continue;
                        }
                        // Since this ticker started (or the camera came
                        // back), so it covers the same window as ticks and
                        // captured.
                        const int64_t baseline =
                            i < m_incompleteBaseline.size() ? m_incompleteBaseline[i] : 0;
                        const int64_t incomplete = std::max<int64_t>(
                            0, m_grabbers[i]->incomplete_frames_total() - baseline);
                        const int64_t ticks  = ticksFired - m_awayBaseline[i];
                        const int64_t missed = ticks - captured;

                        // Which fault this is, rather than assuming. This line
                        // used to say "likely missing trigger broadcasts" every
                        // time without reading the corrupted-frame counter on
                        // the same grabber, so a camera whose images were being
                        // destroyed in transit reported a trigger problem. The
                        // slack for a few late ticks and normal jitter now lives
                        // in classify_frame_shortfall().
                        const auto shortfall =
                            classify_frame_shortfall(ticks, captured, incomplete);
                        if (shortfall != FrameShortfall::None && captured < (ticks * 9) / 10) {
                            QString cause;
                            switch (shortfall) {
                                case FrameShortfall::PacketLoss:
                                    cause = QString(
                                                "%1 of them arrived corrupted (GigE packet "
                                                "loss) — check this camera's bandwidth, cable "
                                                "and network port, not the trigger")
                                                .arg(std::min(incomplete, missed));
                                    break;
                                case FrameShortfall::Both:
                                    cause = QString(
                                                "%1 arrived corrupted (GigE packet loss) and "
                                                "the rest never arrived at all — this link has "
                                                "both a bandwidth/cabling problem and missed "
                                                "triggers")
                                                .arg(std::min(incomplete, missed));
                                    break;
                                case FrameShortfall::MissedTriggers:
                                case FrameShortfall::None:
                                    // MissedTriggers allows some corruption
                                    // (under k_packet_loss_share of the gap),
                                    // so only say "none" when it was none.
                                    cause = incomplete <= 0
                                                ? QStringLiteral(
                                                      "they never arrived at all and none were "
                                                      "corrupted — this camera is missing trigger "
                                                      "broadcasts")
                                                : QString(
                                                      "most never arrived at all (only %1 "
                                                      "corrupted) — this camera is mainly "
                                                      "missing trigger broadcasts")
                                                      .arg(std::min(incomplete, missed));
                                    break;
                            }
                            log_warning(QString("[VideoManager] Camera %1: %2 action-command ticks "
                                                "fired so far but only %3 frames captured (%4 "
                                                "missing); %5.")
                                            .arg(m_targets[i].cameraIndex)
                                            .arg(ticks)
                                            .arg(captured)
                                            .arg(missed)
                                            .arg(cause));
                        }
                    }

                    // Self-correct the firing period as each camera's
                    // achievable_fps() measurement improves over time (see
                    // VideoGrabber::run_pylon_loop()'s own periodic
                    // refresh). Real room-11 testing (2026-08-05) showed a
                    // group armed before any camera had a plausible
                    // measurement yet falls back to configured_fps(), which
                    // can be well above every camera's real triggered-
                    // acquisition ceiling — causing severe (>80%) trigger
                    // loss across the WHOLE group, indefinitely, since
                    // nothing previously ever re-checked once the ticker
                    // was already running. Recomputing here — the exact
                    // same achievable_fps()-preferred-over-configured_fps()
                    // rule arm_and_fire_action_commands() used at arm time
                    // — lets the ticker adapt down to reality within one
                    // diagnostic cycle instead of staying pinned at a rate
                    // none of these cameras can actually sustain for the
                    // rest of the session.
                    std::vector<double> freshTargetFps;
                    freshTargetFps.reserve(m_grabbers.size());
                    for (auto* g : m_grabbers) {
                        if (!g) {
                            continue;
                        }
                        const double achievable = g->achievable_fps();
                        freshTargetFps.push_back(achievable > 0.0 ? achievable
                                                                  : g->configured_fps());
                    }
                    const auto newPeriod = std::chrono::duration<double, std::milli>(
                        action_command_period_ms(freshTargetFps));
                    // Only act on a materially different period — floating-
                    // point noise or sub-percent jitter in a repeated
                    // measurement shouldn't restart the tick cadence every
                    // 5 seconds for the whole session.
                    if (std::abs(newPeriod.count() - m_period.count()) > m_period.count() * 0.05) {
                        log_info(QString("[VideoManager] ActionCommandTicker: adjusting firing "
                                         "period %1 ms -> %2 ms as camera achievable-fps "
                                         "measurements converge.")
                                     .arg(m_period.count(), 0, 'f', 1)
                                     .arg(newPeriod.count(), 0, 'f', 1));
                        m_period = newPeriod;
                    }
                }
            }
        } catch (const std::exception& e) {
            log_error(QString("[VideoManager] ActionCommandTicker: unexpected exception, "
                              "stopping firing for the rest of this session: %1")
                          .arg(QString::fromUtf8(e.what())));
        } catch (...) {
            log_error(
                "[VideoManager] ActionCommandTicker: unknown non-standard exception, "
                "stopping firing for the rest of this session.");
        }
    }

   private:
    std::unique_ptr<ActionCommandSession> m_session;
    std::vector<ActionCommandTarget> m_targets;
    std::vector<VideoGrabber*> m_grabbers;
    std::unique_ptr<ActionTickLog> m_tickLog; // null for the preview ticker

   public:
    // Only after a forced terminate() — see VideoManager::stop_action_ticker().
    // Deliberately leaks the log object rather than destroy (and so flush) a
    // stream the killed thread may have been writing to.
    void abandon_tick_log() { (void)m_tickLog.release(); }

   private:
    // incomplete_frames_total() per camera at construction — see the ctor for
    // why this one counter needs a baseline and the other two do not.
    std::vector<int64_t> m_incompleteBaseline;
    // Per camera, for one that dropped out and came back: the shortfall
    // (ticks minus frames) at the first check after it returned, which the
    // missed-trigger diagnostic subtracts — see run(). Zero otherwise.
    std::vector<int64_t> m_awayBaseline;
    // Each camera's VideoGrabber::reconnects() as last seen by run().
    std::vector<int64_t> m_seenReconnects;
    std::chrono::duration<double, std::milli> m_period;
    std::atomic<int64_t> m_ticksFired{0};
};

struct CameraUnit {
    std::unique_ptr<RingBuffer<std::shared_ptr<VideoFrame>>> buffer;
    std::unique_ptr<VideoGrabber> grabber;
    std::unique_ptr<VideoEncoder> encoder;
    bool encoderDone{false};
    // Position in the *configured* settings.cameras array this unit was
    // opened from — NOT necessarily this unit's position in d->units, since
    // an earlier camera that failed to open leaves a gap. VideoGrabber keeps
    // this same value as its own cameraIndex (used for the frame_preview
    // signal, and to name video_N.mp4/timestamps_camN.csv), so anything that
    // needs to re-correlate a unit back to its CameraParameters must use
    // this field rather than the unit's position in d->units.
    int configIndex{0};
};

struct VideoManager::Impl {
    std::vector<CameraUnit> units;
    bool recording{false};
    bool previewing{false};
    std::atomic<int> stoppedCount{0};
    int cameraCount{0};

    std::atomic<int64_t> totalEncoded{0};
    std::atomic<int64_t> totalDropped{0};

    // Owns the background thread continuously firing GigE Vision Action
    // Commands while any Action1-armed camera is grabbing (preview or
    // recording). Only non-null while at least one such camera is running
    // — see arm_and_fire_action_commands()/stop_action_ticker().
    std::unique_ptr<ActionCommandTicker> actionTicker;

    // Snapshot of the most recently stopped actionTicker's own ticks_fired()
    // — see VideoManager::action_ticks_fired()'s doc comment. -1 = Action1
    // triggering was never used in this VideoManager's lifetime.
    int64_t lastActionTicksFired = -1;

    // Post-recording snapshots — see VideoManager::last_recording_snapshot()'s
    // doc comment for why a live read is unusable from a recording_stopped
    // handler.
    std::vector<VideoManager::RecordingCameraSnapshot> lastRecordingSnapshot;
    int64_t lastRecordingActionTicks = -1;

    // Where the next ticker started by arm_and_fire_action_commands() writes
    // its tick log; empty = no log. Set by start() for the recording's ticker
    // only, and cleared straight after, so the preview ticker that resumes
    // once recording stops never writes into the finished session.
    QString tickLogDir;

    // ── Interview mode ────────────────────────────────────────────────────
    // The settings object open() was given — an AppSettings member, alive for
    // the app's lifetime. Needed by apply_live_params(), which is handed only
    // an index but must rebuild interviewParams from the camera's own entry.
    const VideoSettings* openedSettings{nullptr};
    // What the interview camera's grabber is bound to while interview mode is
    // open, null otherwise. A VideoGrabber keeps a `const CameraParameters&`
    // for its whole lifetime (see VideoSettings::kMaxCameras), so the merged
    // parameters cannot be a temporary; heap-held so the address survives any
    // move of Impl's members. Reset only after the grabber bound to it is gone.
    std::unique_ptr<CameraParameters> interviewParams;
    int interviewIndex{-1};
    // The interview settings open() used. Live edits rebuild interviewParams
    // from this, not from openedSettings->interview: that can be edited and
    // applied while a reopen is deferred, and those values must not leak into
    // a camera still running the old crop and rate.
    InterviewSettings openedInterview;
};

VideoManager::VideoManager(QObject* parent) : QObject(parent), d(std::make_unique<Impl>()) {}

VideoManager::~VideoManager() {
    stop();
    close();
}

// ── Open / Close ───────────────────────────────────────────────────────────

int VideoManager::open(const VideoSettings& settings) {
    close();

    // Interview mode opens one camera with its own crop and rate laid over
    // that camera's configuration — see interview_camera_params(). The rest
    // stay closed rather than open-but-idle: an idle open camera still holds
    // its GigE link and its Pylon device, and would still be armed by
    // start_preview().
    d->openedSettings = &settings;
    d->interviewIndex = settings.interview_active() ? settings.interview.cameraIndex : -1;
    if (d->interviewIndex >= 0) {
        d->openedInterview = settings.interview;
        d->interviewParams = std::make_unique<CameraParameters>(interview_camera_params(
            settings.cameras[static_cast<size_t>(d->interviewIndex)], settings.interview));
        log_info(QString("[VideoManager] open: interview mode — %1 only, %2\xd7%3+%4+%5 @ %6 fps, "
                         "free-running (no hardware trigger)")
                     .arg(camera_label(d->interviewIndex))
                     .arg(d->interviewParams->width)
                     .arg(d->interviewParams->height)
                     .arg(d->interviewParams->offsetX)
                     .arg(d->interviewParams->offsetY)
                     .arg(d->interviewParams->fps));
    }

    // Guard against ambiguous camera identity: VideoGrabber attaches to a
    // device by serial number, falling back to Pylon's "first device found"
    // when serialNumber is empty (video_grabber.cpp). The actual collision
    // risk is between two or more empty-serial cameras (both could attach
    // to the same "first" enumerated device) or between two cameras sharing
    // an explicit serial — a single empty-serial camera alongside others
    // that are each pinned to a specific serial is not ambiguous, so it is
    // deliberately not flagged here.
    QSet<QString> seenSerials;
    QSet<QString> duplicateSerials;
    int emptySerialCount = 0;
    for (const auto& cam : settings.cameras) {
        if (cam.serialNumber.isEmpty()) {
            ++emptySerialCount;
            continue;
        }
        if (seenSerials.contains(cam.serialNumber)) {
            duplicateSerials.insert(cam.serialNumber);
        }
        seenSerials.insert(cam.serialNumber);
    }
    const bool ambiguousEmpty = emptySerialCount > 1;

    int opened = 0;
    d->units.reserve(settings.cameras.size());

    for (int i = 0; i < static_cast<int>(settings.cameras.size()); ++i) {
        // `continue` rather than filtering the list: unit.configIndex must
        // stay the camera's configured index, which names its files.
        if (d->interviewIndex >= 0 && i != d->interviewIndex) {
            continue;
        }
        const auto& cam =
            d->interviewIndex >= 0 ? *d->interviewParams : settings.cameras[static_cast<size_t>(i)];

        // A shared serial only matters when two grabbers could attach to the
        // one device, which cannot happen with one camera opened. An empty
        // serial still does: "first device found" is then any camera in the
        // room, so the guard below stays as it was.
        const bool singleCamera = d->interviewIndex >= 0;
        if (!singleCamera && !cam.serialNumber.isEmpty() &&
            duplicateSerials.contains(cam.serialNumber)) {
            log_error(QString("[VideoManager] open: cam %1 serial '%2' is used by more than "
                              "one configured camera — skipping to avoid two grabbers "
                              "attaching to the same physical device. Fix the serial numbers "
                              "in the Video settings.")
                          .arg(i)
                          .arg(cam.serialNumber));
            continue;
        }
        if (cam.serialNumber.isEmpty() && ambiguousEmpty) {
            log_error(QString("[VideoManager] open: cam %1 has no serial number configured, "
                              "and more than one configured camera is missing a serial — "
                              "skipping rather than risk two grabbers attaching to the same "
                              "\"first device found\". Set this camera's serial number in the "
                              "Video settings.")
                          .arg(i));
            continue;
        }

        auto unit        = CameraUnit{};
        unit.configIndex = i;
        unit.buffer  = std::make_unique<RingBuffer<std::shared_ptr<VideoFrame>>>(k_ring_capacity);
        unit.grabber = std::make_unique<VideoGrabber>(i, cam, *unit.buffer);

        connect(unit.grabber.get(), &VideoGrabber::opened, this, &VideoManager::camera_opened,
                Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::closed, this, &VideoManager::camera_closed,
                Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::frame_dropped, this,
                &VideoManager::frame_dropped, Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::grab_error, this, &VideoManager::camera_error,
                Qt::QueuedConnection);

        connect(unit.grabber.get(), &VideoGrabber::preview_frame, this,
                &VideoManager::frame_preview, Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::calibration_frame_ready, this,
                &VideoManager::calibration_frame_ready, Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::action_command_capability, this,
                &VideoManager::action_command_capability, Qt::QueuedConnection);
        // Queued is mandatory here, not just conventional: unlike the
        // capability signal above, this one is emitted from the grab thread.
        connect(unit.grabber.get(), &VideoGrabber::achievable_fps_changed, this,
                &VideoManager::achievable_fps_changed, Qt::QueuedConnection);

        connect(
            unit.grabber.get(), &VideoGrabber::frame_dropped, this,
            [this](int /*cam*/, int64_t /*id*/) { d->totalDropped.fetch_add(1); },
            Qt::QueuedConnection);

        log_info(QString("[VideoManager] open: calling open() for cam %1 (serial %2)")
                     .arg(i)
                     .arg(cam.serialNumber));
        if (unit.grabber->open()) {
            log_info(QString("[VideoManager] open: cam %1 open() returned, pushing unit").arg(i));
            d->units.push_back(std::move(unit));
            log_info(QString("[VideoManager] open: cam %1 unit pushed, opened=%2")
                         .arg(i)
                         .arg(opened + 1));
            ++opened;
        } else {
            log_warning(QString("[VideoManager] open: cam %1 open() failed").arg(i));
        }
    }

    d->cameraCount = opened;
    log_info(QString("[VideoManager] %1 of %2 camera(s) opened.")
                 .arg(opened)
                 .arg(d->interviewIndex >= 0 ? 1 : static_cast<int>(settings.cameras.size())));
    return opened;
}

void VideoManager::close() {
    log_info(QString("[VideoManager] close() units=%1 rec=%2 prev=%3")
                 .arg(d->units.size())
                 .arg(d->recording)
                 .arg(d->previewing));
    if (d->recording) {
        stop();
    } else {
        stop_action_ticker();
        d->previewing = false;
        for (int i = 0; i < static_cast<int>(d->units.size()); ++i) {
            auto& unit = d->units[static_cast<size_t>(i)];
            if (unit.grabber) {
                log_info(QString("[VideoManager] close: stop_grabbing cam %1").arg(i));
                unit.grabber->stop_grabbing();
                log_info(QString("[VideoManager] close: cam %1 thread stopped").arg(i));
            }
        }
    }
    log_info("[VideoManager] close: closing Pylon devices");
    for (auto& unit : d->units) {
        if (unit.grabber) {
            unit.grabber->close();
            // Retract this camera's measured rate: a closed camera has no
            // achievable rate, and leaving the last reading on screen would
            // present a measurement from a session that has ended as if it
            // still applied. The UI falls back to its awaiting-measurement /
            // exposure-ceiling state (see compute_fps_readout()).
            emit achievable_fps_changed(unit.configIndex, -1.0);
        }
    }
    // Flush any queued MetaCallEvents (preview_frame, closed) that were posted
    // to this object by the now-stopped grabbers.  Removing them before
    // destroying the grabber objects prevents Qt from delivering events that
    // reference sender objects that are about to become dangling.
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    log_info("[VideoManager] close: clearing units");
    d->units.clear();
    d->cameraCount = 0;
    // Only now: the interview grabber held a reference to these until the
    // units.clear() above destroyed it.
    d->interviewParams.reset();
    d->interviewIndex = -1;
    log_info("[VideoManager] close: done");
}

// ── Recording lifecycle ────────────────────────────────────────────────────

void VideoManager::start_preview() {
    if (d->units.empty()) {
        return;
    }
    arm_and_fire_action_commands();
    d->previewing = true;
    log_info(QString("[VideoManager] Preview started for %1 camera(s).").arg(d->units.size()));
}

void VideoManager::start(const QString& sessionDir, const QString& videoBasename,
                         const VideoSettings& settings) {
    if (d->recording || d->units.empty()) {
        return;
    }

    // Stop preview grabbers so ring buffers can be safely reset before recording.
    if (d->previewing) {
        stop_action_ticker();
        for (auto& unit : d->units) {
            if (unit.grabber && unit.grabber->isRunning()) {
                unit.grabber->stop_grabbing();
                unit.grabber->wait(5000);
            }
        }
        d->previewing = false;
    }

    d->totalEncoded.store(0);
    d->totalDropped.store(0);
    d->stoppedCount.store(0);

    // Pass 1: prepare every camera's encoder (ring buffer reset, encoder
    // construction, start_encoding()) without starting any grabber yet.
    // Pass 2 then starts all 6 grabbers back-to-back, so the first-frame
    // time of each camera is not skewed by the setup cost of the others —
    // that skew previously accumulated when prep and grab-start were
    // interleaved camera-by-camera in a single loop.
    for (auto& unit : d->units) {
        // Use this unit's original config-array position, not its position
        // in d->units — those diverge as soon as any earlier camera fails
        // to open, and pairing positionally here would silently apply the
        // wrong camera's width/height/fps and write to the wrong filename.
        const int i = unit.configIndex;
        // The parameters this camera was opened with — the interview overlay,
        // not the room configuration, when interview mode is open. Read from
        // what open() bound rather than re-derived from `settings`, so the
        // encoder can never disagree with the grabber about the rate or crop.
        const CameraParameters fallback{};
        const auto& cam = (i == d->interviewIndex && d->interviewParams) ? *d->interviewParams
                          : (i < static_cast<int>(settings.cameras.size()))
                              ? settings.cameras[static_cast<size_t>(i)]
                              : fallback;

        const QString suffix    = QString("_%1").arg(i);
        const QString videoPath = sessionDir + "/" + videoBasename + suffix + ".mp4";
        const QString tsPath    = sessionDir + "/timestamps_cam" + QString::number(i) + ".csv";

        // Clear stale preview frames so the encoder starts from a clean buffer.
        unit.buffer->reset(k_ring_capacity);

        VideoEncoder::Config cfg;
        cfg.cameraIndex   = i;
        cfg.outputPath    = videoPath;
        cfg.timestampPath = tsPath;
        cfg.codec         = settings.codec;
        cfg.preset        = settings.preset;
        cfg.bitrate       = settings.bitrate;
        cfg.crf           = settings.crf;
        // Sized from what the camera actually delivers when it was read back:
        // a camera that adjusted the requested crop to its own step would
        // otherwise hand the encoder frames of a different size than it was
        // built for. See VideoGrabber::frame_width().
        cfg.width  = unit.grabber->frame_width() > 0 ? unit.grabber->frame_width() : cam.width;
        cfg.height = unit.grabber->frame_height() > 0 ? unit.grabber->frame_height() : cam.height;
        cfg.fps    = cam.fps;

        unit.encoder = std::make_unique<VideoEncoder>(cfg, *unit.buffer);

        connect(unit.encoder.get(), &VideoEncoder::encoding_stopped, this,
                &VideoManager::on_encoder_stopped, Qt::QueuedConnection);
        connect(unit.encoder.get(), &VideoEncoder::encoding_error, this,
                &VideoManager::camera_error, Qt::QueuedConnection);

        unit.encoder->start_encoding();

        log_info(QString("[VideoManager] Camera %1 recording → %2").arg(i).arg(videoPath));
    }

    // Pass 2 (continued): arm every camera (start_grabbing()) and, for any
    // configured for GigE Vision Action1 triggering, fire the action-command
    // broadcasts only after every armed camera has confirmed start_grabbing()
    // returned — see arm_and_fire_action_commands(). This ordering (slow arm
    // step fully done before the fast, tightly-timed fire step) is what
    // delivers materially tighter cross-camera simultaneity than a single
    // interleaved start_grabbing() loop would.
    //
    // This ticker — the recording's — logs every tick into the session, so
    // frames can later be placed on the trigger that produced them.
    d->tickLogDir = sessionDir;
    arm_and_fire_action_commands();
    d->tickLogDir.clear();

    d->recording = true;
}

void VideoManager::stop() {
    if (!d->recording) {
        return;
    }
    d->recording = false;

    stop_action_ticker();

    // Signal grabbers to stop producing — encoders will drain and finish on their own.
    for (auto& unit : d->units) {
        if (unit.grabber) {
            unit.grabber->stop_grabbing();
        }
        if (unit.encoder) {
            unit.encoder->stop_encoding();
        }
    }
    QSet<int> unfinishedEncoders; // config indices whose encoder outlived the wait
    // Wait for all threads to exit. A timed-out wait matters for the snapshot
    // below: an encoder still draining reports fewer framesEncoded than were
    // grabbed, which would otherwise be read as ground truth.
    for (auto& unit : d->units) {
        if (unit.grabber && !unit.grabber->wait(5000)) {
            log_warning(QString("[VideoManager] Camera %1 grabber did not exit within 5s — its "
                                "health-report counters may be incomplete.")
                            .arg(unit.configIndex));
        }
        if (unit.encoder && !unit.encoder->wait(10000)) {
            unfinishedEncoders.insert(unit.configIndex);
            log_warning(QString("[VideoManager] Camera %1 encoder did not finish draining within "
                                "10s — framesEncoded may under-report.")
                            .arg(unit.configIndex));
        }
    }

    // Latch every camera's final counters before anything can reset them.
    // stop_grabbing() leaves them intact, but the preview restart that follows
    // recording_stopped calls start_grabbing(), which zeroes frameCounter/
    // dropCounter — and that restart is wired up in Application::initialize()
    // before MainWindow exists, so it runs first. Without this snapshot every
    // post-recording consumer reads zeros. Taken after the threads have joined
    // so the encoders' drain is included in framesEncoded.
    d->lastRecordingSnapshot.clear();
    d->lastRecordingSnapshot.reserve(d->units.size());
    for (const auto& unit : d->units) {
        if (!unit.grabber) {
            continue;
        }
        RecordingCameraSnapshot snap;
        snap.configIndex        = unit.configIndex;
        snap.framesGrabbed      = unit.grabber->frames_grabbed();
        snap.framesDropped      = unit.grabber->frames_dropped();
        snap.incompleteFrames   = unit.grabber->incomplete_frames_total();
        snap.configuredFps      = unit.grabber->configured_fps();
        snap.achievableFps      = unit.grabber->achievable_fps();
        snap.actionCommandReady = unit.grabber->action_command_ready();
        snap.framesEncoded      = unit.encoder ? unit.encoder->frames_encoded() : 0;
        snap.encoderFinished    = !unfinishedEncoders.contains(unit.configIndex);
        if (unit.encoder) {
            snap.firstFrameElapsedNs = unit.encoder->first_frame_elapsed_ns();
            snap.lastFrameElapsedNs  = unit.encoder->last_frame_elapsed_ns();
        }
        d->lastRecordingSnapshot.push_back(snap);
    }
    // stop_action_ticker() above already latched the ticker's own count into
    // lastActionTicksFired; copy it aside before the preview restart resets it.
    d->lastRecordingActionTicks = d->lastActionTicksFired;
}

const std::vector<VideoManager::RecordingCameraSnapshot>& VideoManager::last_recording_snapshot()
    const {
    return d->lastRecordingSnapshot;
}

int64_t VideoManager::last_recording_action_ticks() const { return d->lastRecordingActionTicks; }

void VideoManager::clear_recording_snapshot() {
    d->lastRecordingSnapshot.clear();
    d->lastRecordingActionTicks = -1;
}

void VideoManager::apply_live_params(int configIndex) {
    // The interview grabber reads a merged copy, not cameras[configIndex], so
    // an edit made on that camera's card must be merged in again before the
    // grabber re-applies, or the card would appear to do nothing. Done here,
    // on the GUI thread, before the grabber is told to re-read: the same
    // ordering every other camera's live edit already relies on.
    if (configIndex == d->interviewIndex && d->interviewParams && d->openedSettings &&
        configIndex < static_cast<int>(d->openedSettings->cameras.size())) {
        *d->interviewParams = interview_camera_params(
            d->openedSettings->cameras[static_cast<size_t>(configIndex)], d->openedInterview);
    }
    for (auto& unit : d->units) {
        if (unit.configIndex == configIndex && unit.grabber) {
            unit.grabber->apply_live_params();
            return;
        }
    }
}

void VideoManager::request_calibration_frame(int configIndex, uint64_t token) {
    for (auto& unit : d->units) {
        if (unit.configIndex == configIndex && unit.grabber) {
            unit.grabber->request_calibration_frame(token);
            return;
        }
    }
}

void VideoManager::arm_and_fire_action_commands() {
    // Reset up front, before deciding whether a ticker is even needed this
    // call — otherwise a session that doesn't use Action1 triggering would
    // silently keep reporting an earlier session's tick count via
    // action_ticks_fired() (see that accessor's own doc comment), since
    // stop_action_ticker() only snapshots a fresh value when a ticker
    // actually existed to snapshot.
    d->lastActionTicksFired = -1;

    // Identify every unit eligible to be freshly armed this call, and the
    // Action1-ready subset among them.
    std::vector<CameraUnit*> pending;       // all not-yet-running units
    std::vector<CameraUnit*> actionTargets; // subset that are Action1-ready
    for (auto& unit : d->units) {
        if (!unit.grabber || unit.grabber->isRunning()) {
            continue;
        } // already armed
        pending.push_back(&unit);
        if (unit.grabber->action_command_ready()) {
            actionTargets.push_back(&unit);
        }
    }

    // Phase 1 (arm): start every pending grabber's thread. Cameras
    // configured for Action1 are now parked waiting for FrameStart
    // triggers (see VideoGrabber::open()) — nothing arrives until the
    // ticker below starts.
    for (auto* u : pending) {
        u->grabber->start_grabbing();
    }
    if (actionTargets.empty()) {
        return;
    }

    // start_grabbing() only confirms QThread::start() was scheduled, not that
    // Pylon's StartGrabbing() has actually run yet (that call does real
    // node-map I/O first) — firing immediately would let the ticker's first
    // tick or two reach a camera whose stream grabber is not armed. Poll
    // briefly, bounded so a slow or stuck camera can never turn this into an
    // indefinite wait: "degrade safely, never hang", and a camera still not
    // armed when the timeout expires just gets whatever tick catches it next.
    //
    // Be precise about what this does and does not buy, because the difference
    // has already misled one investigation. It guarantees that every armed
    // camera's StartGrabbing() has returned before the first broadcast. It
    // cannot guarantee that every camera's first *frame* lands with the group:
    // between StartGrabbing() returning and a frame arriving, the camera still
    // has to receive the trigger, expose, transmit, and have its packets
    // survive the wire. On a link that is losing packets those first frames
    // die in transit and that camera simply appears late, with the barrier
    // having passed cleanly.
    //
    // Measured on room 11, 2026-09-07: with this barrier satisfied, Camera 2
    // (config index 1) still delivered its first frame 264 ms (3.4 trigger
    // periods) after the rest. It was the only camera logging incomplete
    // frames that session, and once started it ran with zero gaps and ~1 ms of
    // hardware-timestamp jitter. A late first frame is evidence about that
    // camera's link, not about this wait being too short; a longer timeout
    // would not have moved it.
    {
        constexpr int k_readinessPollTimeoutMs  = 500;
        constexpr int k_readinessPollIntervalMs = 2;
        int waitedMs                            = 0;
        while (waitedMs < k_readinessPollTimeoutMs) {
            const bool allReady =
                std::all_of(actionTargets.begin(), actionTargets.end(),
                            [](CameraUnit* u) { return u->grabber->is_actually_grabbing(); });
            if (allReady) {
                break;
            }
            QThread::msleep(k_readinessPollIntervalMs);
            waitedMs += k_readinessPollIntervalMs;
        }
        if (waitedMs >= k_readinessPollTimeoutMs) {
            log_warning(
                "[VideoManager] Timed out waiting for all Action1-armed cameras to "
                "actually start grabbing — firing anyway; any camera not yet ready "
                "will pick up on a later tick instead of the first one.");
        }
    }

    // Phase 2 (fire): start a fresh ticker that continuously fires one
    // Action Command per frame for as long as it runs — see
    // ActionCommandTicker's own doc comment for why a per-frame trigger,
    // not a single one-shot broadcast, is required on this camera
    // generation. Stop any previous ticker first (shouldn't normally still
    // be running here — every caller stops it before re-arming — but
    // defensive, since starting two tickers would double-fire).
    stop_action_ticker();

    std::vector<ActionCommandTarget> targets;
    std::vector<VideoGrabber*> grabbers; // parallel to targets — see ActionCommandTicker
    std::vector<double> targetFps;
    QStringList targetDesc;
    for (auto* u : actionTargets) {
        targets.push_back({u->configIndex, u->grabber->action_device_key(),
                           u->grabber->action_broadcast_address()});
        grabbers.push_back(u->grabber.get());
        // Prefer the camera's real, measured achievable rate over the
        // requested one — a camera GigE-bandwidth/exposure/ROI-limited
        // below its configured fps would otherwise get triggered faster
        // than it can actually process, reproducing the exact packet-loss
        // failure mode this project already knows about from over-requested
        // free-run fps. Falls back to configured_fps() if never measured
        // (achievable_fps() < 0 — stub builds, or the node was unavailable).
        const double achievable = u->grabber->achievable_fps();
        targetFps.push_back(achievable > 0.0 ? achievable : u->grabber->configured_fps());
        targetDesc << QString("cam%1@%2").arg(u->configIndex).arg(targets.back().broadcastAddress);
    }
    const double periodMs = action_command_period_ms(targetFps);

    // Construct (and validate) the transport-layer session HERE, on the
    // main thread, before starting any background thread — not inside the
    // ticker's own run(). A failure here (e.g. the GigE transport layer
    // itself unavailable — a machine/driver-level problem, not specific to
    // any one camera) previously surfaced as nothing but a background-thread
    // log line: every Action1-armed camera would sit parked, producing zero
    // frames, completely indistinguishable from working correctly. Checking
    // it here lets us fail loudly and per-camera instead.
    auto session = std::make_unique<ActionCommandSession>();
    if (!session->is_valid()) {
        const QString msg =
            "GigE transport layer unavailable — Action Command triggering "
            "cannot start this session; this camera will produce zero frames "
            "until Action1 is deselected or the camera is reopened.";
        log_error(QString("[VideoManager] %1").arg(msg));
        for (auto* u : actionTargets) {
            emit camera_error(u->configIndex, msg);
        }
        return;
    }

    log_info(QString("[VideoManager] Starting continuous GigE Action Command firing "
                     "(every %1 ms) for %2 camera(s): %3")
                 .arg(periodMs, 0, 'f', 1)
                 .arg(targets.size())
                 .arg(targetDesc.join(", ")));
    // The recording's tick log, plus which cameras the ticks apply to: a
    // camera outside the Action1 group free-runs, and must not be forced onto
    // a trigger grid it never followed.
    std::unique_ptr<ActionTickLog> tickLog;
    if (!d->tickLogDir.isEmpty()) {
        tickLog = std::make_unique<ActionTickLog>();
        if (tickLog->open(d->tickLogDir + "/action_ticks.csv")) {
            QJsonArray cams;
            for (const auto& t : targets) cams.append(t.cameraIndex);
            const QJsonObject group{
                {"tick_log", "action_ticks.csv"},
                {"cameras", cams},
                {"initial_period_ms", periodMs},
                {"margin", k_default_action_margin},
            };
            QFile f(d->tickLogDir + "/action_group.json");
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                f.write(QJsonDocument(group).toJson(QJsonDocument::Indented));
            }
        } else {
            tickLog.reset();
        }
    }
    d->actionTicker = std::make_unique<ActionCommandTicker>(
        std::move(session), std::move(targets), std::move(grabbers), periodMs, std::move(tickLog));
    d->actionTicker->start();
}

void VideoManager::stop_action_ticker() {
    if (!d->actionTicker) {
        return;
    }
    d->actionTicker->requestInterruption();
    if (!d->actionTicker->wait(5000)) {
        log_warning("[VideoManager] ActionCommandTicker did not finish in 5 s — forcing terminate");
        d->actionTicker->terminate();
        d->actionTicker->wait();
        // Killed possibly mid-write: its tick log's stream is in an unknown
        // state, and flushing it on destruction could write garbage or crash.
        // Abandon it instead — the loader stops at the last complete row.
        d->actionTicker->abandon_tick_log();
    }
    // Snapshot before destroying — see action_ticks_fired()'s doc comment.
    d->lastActionTicksFired = d->actionTicker->ticks_fired();
    d->actionTicker.reset();
}

void VideoManager::on_encoder_stopped(int cameraIndex, int64_t frames) {
    d->totalEncoded.fetch_add(frames);
    log_info(
        QString("[VideoManager] Camera %1 encoder done (%2 frames).").arg(cameraIndex).arg(frames));
    const int done = d->stoppedCount.fetch_add(1) + 1;
    if (done >= d->cameraCount) {
        emit recording_stopped();
    }
}

// ── Accessors ──────────────────────────────────────────────────────────────

bool VideoManager::is_recording() const { return d->recording; }
bool VideoManager::is_previewing() const { return d->previewing; }
int VideoManager::camera_count() const { return d->cameraCount; }
int64_t VideoManager::total_frames_encoded() const { return d->totalEncoded.load(); }
int64_t VideoManager::total_frames_dropped() const { return d->totalDropped.load(); }
int64_t VideoManager::action_ticks_fired() const { return d->lastActionTicksFired; }

bool VideoManager::camera_action_command_ready(int index) const {
    if (index < 0 || index >= static_cast<int>(d->units.size())) {
        return false;
    }
    const auto& unit = d->units[static_cast<size_t>(index)];
    return unit.grabber && unit.grabber->action_command_ready();
}

VideoManager::CameraStats VideoManager::camera_stats_for_config_index(int configIndex) const {
    for (std::size_t p = 0; p < d->units.size(); ++p) {
        if (d->units[p].configIndex == configIndex) {
            return camera_stats(static_cast<int>(p));
        }
    }
    return {}; // configured but not open — grabberRunning stays false
}

int VideoManager::interview_camera_index() const { return d->interviewIndex; }

double VideoManager::camera_max_fps(int configIndex) const {
    for (const auto& unit : d->units) {
        if (unit.configIndex == configIndex && unit.grabber) {
            // The camera's current figure once it has one — refreshed while
            // grabbing and after live edits (exposure) — else what it said at
            // open, which is all there is straight after a reopen.
            const double current = unit.grabber->achievable_fps();
            return current > 0 ? current : unit.grabber->camera_max_fps();
        }
    }
    return -1.0;
}

std::optional<VideoManager::OpenedGeometry> VideoManager::opened_geometry(int configIndex) const {
    for (const auto& unit : d->units) {
        if (unit.configIndex == configIndex && unit.grabber) {
            OpenedGeometry g;
            g.width       = unit.grabber->frame_width();
            g.height      = unit.grabber->frame_height();
            g.offsetX     = unit.grabber->frame_offset_x();
            g.offsetY     = unit.grabber->frame_offset_y();
            g.pixelFormat = unit.grabber->pixel_format();
            if (g.width <= 0 || g.height <= 0) {
                return std::nullopt;
            }
            return g;
        }
    }
    return std::nullopt;
}

int VideoManager::camera_config_index(int position) const {
    if (position < 0 || position >= static_cast<int>(d->units.size())) {
        return -1;
    }
    return d->units[static_cast<size_t>(position)].configIndex;
}

VideoManager::CameraStats VideoManager::camera_stats(int index) const {
    CameraStats stats;
    if (index < 0 || index >= static_cast<int>(d->units.size())) {
        return stats;
    }
    const auto& unit = d->units[static_cast<size_t>(index)];
    if (unit.grabber) {
        stats.fps                = unit.grabber->current_fps();
        stats.framesGrabbed      = unit.grabber->frames_grabbed();
        stats.framesDropped      = unit.grabber->frames_dropped();
        stats.grabberRunning     = unit.grabber->isRunning();
        stats.reconnecting       = unit.grabber->is_reconnecting();
        stats.lastFrameElapsedNs = unit.grabber->last_frame_elapsed_ns();
        stats.configuredFps      = unit.grabber->configured_fps();
        stats.achievableFps      = unit.grabber->achievable_fps();
        stats.incompleteFrames   = unit.grabber->incomplete_frames_total();
    }
    if (unit.encoder) {
        stats.framesEncoded = unit.encoder->frames_encoded();
    }
    if (unit.buffer) {
        const auto avail  = unit.buffer->available_read();
        const auto cap    = unit.buffer->capacity();
        stats.ringFillPct = (cap > 0) ? static_cast<int>((avail * 100ULL) / cap) : 0;
    }
    return stats;
}

} // namespace mosaic
