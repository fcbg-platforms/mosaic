#include "video/video_manager.hpp"
#include "trigger/trigger_manager.hpp"
#include "utils/ring_buffer.hpp"
#include "utils/timestamp.hpp"
#include "video/gige_action_command.hpp"
#include "video/video_encoder.hpp"
#include "video/video_grabber.hpp"
#include "utils/logger.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

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
    ActionCommandTicker(std::unique_ptr<ActionCommandSession> session,
                         std::vector<ActionCommandTarget> targets,
                         std::vector<VideoGrabber*> grabbers, double periodMs)
        : m_session(std::move(session)), m_targets(std::move(targets))
        , m_grabbers(std::move(grabbers))
        , m_period(std::chrono::duration<double, std::milli>(periodMs)) {}

protected:
    void run() override {
        // ActionCommandSession::fire() already catches per-target
        // Pylon::GenericException internally, but this outer net catches
        // anything else — an uncaught exception escaping QThread::run() is
        // fatal to the whole process (std::terminate()), and this thread is
        // meant to run unattended for an entire recording session.
        try {
            auto nextTick = SteadyClock::now();
            auto lastDiagTime = nextTick;
            int64_t ticksFired = 0;
            while (!isInterruptionRequested()) {
                const auto now = SteadyClock::now();
                if (now < nextTick) {
                    QThread::msleep(1);
                    continue;
                }
                nextTick += std::chrono::duration_cast<SteadyClock::duration>(m_period);
                const int fired = m_session->fire(m_targets, k_action_group_key, k_action_group_mask);
                ++ticksFired;
                if (fired < static_cast<int>(m_targets.size())) {
                    log_warning(QString("[VideoManager] ActionCommandTicker: only %1/%2 action-"
                                        "command broadcasts succeeded this tick.")
                                    .arg(fired).arg(m_targets.size()));
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
                // one that never arrived at all.
                if (std::chrono::duration<double>(now - lastDiagTime).count() >= 5.0) {
                    lastDiagTime = now;
                    for (size_t i = 0; i < m_targets.size() && i < m_grabbers.size(); ++i) {
                        if (!m_grabbers[i]) { continue; }
                        const int64_t captured = m_grabbers[i]->frames_grabbed();
                        // A little slack: the readiness barrier still lets a
                        // camera join a few ticks late, and a healthy camera
                        // can lag by a frame or two under normal jitter —
                        // only flag a gap large enough to mean real,
                        // sustained missed triggers, not startup noise.
                        const int64_t missed = ticksFired - captured;
                        if (missed > 5 && captured < (ticksFired * 9) / 10) {
                            log_warning(QString("[VideoManager] Camera %1: %2 action-command "
                                                "ticks fired so far but only %3 frames captured "
                                                "(%4 missing) — this camera is likely missing "
                                                "trigger broadcasts, not just corrupted frames.")
                                            .arg(m_targets[i].cameraIndex)
                                            .arg(ticksFired).arg(captured).arg(missed));
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
                        if (!g) { continue; }
                        const double achievable = g->achievable_fps();
                        freshTargetFps.push_back(achievable > 0.0 ? achievable : g->configured_fps());
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
            log_error("[VideoManager] ActionCommandTicker: unknown non-standard exception, "
                      "stopping firing for the rest of this session.");
        }
    }

private:
    std::unique_ptr<ActionCommandSession> m_session;
    std::vector<ActionCommandTarget>      m_targets;
    std::vector<VideoGrabber*>            m_grabbers;
    std::chrono::duration<double, std::milli> m_period;
};

struct CameraUnit {
    std::unique_ptr<RingBuffer<std::shared_ptr<VideoFrame>>> buffer;
    std::unique_ptr<VideoGrabber>                            grabber;
    std::unique_ptr<VideoEncoder>                            encoder;
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
    bool                    recording{false};
    bool                    previewing{false};
    std::atomic<int>        stoppedCount{0};
    int                     cameraCount{0};
    TriggerManager*         triggerMgr{nullptr};  // not owned, may be null

    std::atomic<int64_t>    totalEncoded{0};
    std::atomic<int64_t>    totalDropped{0};

    // Owns the background thread continuously firing GigE Vision Action
    // Commands while any Action1-armed camera is grabbing (preview or
    // recording). Only non-null while at least one such camera is running
    // — see arm_and_fire_action_commands()/stop_action_ticker().
    std::unique_ptr<ActionCommandTicker> actionTicker;
};

VideoManager::VideoManager(TriggerManager* triggerMgr, QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>()) {
    d->triggerMgr = triggerMgr;
}

VideoManager::~VideoManager() { stop(); close(); }

// ── Open / Close ───────────────────────────────────────────────────────────

int VideoManager::open(const VideoSettings& settings) {
    close();

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
        if (cam.serialNumber.isEmpty()) { ++emptySerialCount; continue; }
        if (seenSerials.contains(cam.serialNumber)) { duplicateSerials.insert(cam.serialNumber); }
        seenSerials.insert(cam.serialNumber);
    }
    const bool ambiguousEmpty = emptySerialCount > 1;

    int opened = 0;
    d->units.reserve(settings.cameras.size());

    for (int i = 0; i < static_cast<int>(settings.cameras.size()); ++i) {
        const auto& cam = settings.cameras[static_cast<size_t>(i)];

        if (!cam.serialNumber.isEmpty() && duplicateSerials.contains(cam.serialNumber)) {
            log_error(QString("[VideoManager] open: cam %1 serial '%2' is used by more than "
                               "one configured camera — skipping to avoid two grabbers "
                               "attaching to the same physical device. Fix the serial numbers "
                               "in the Video settings.")
                          .arg(i).arg(cam.serialNumber));
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

        auto unit       = CameraUnit{};
        unit.configIndex = i;
        unit.buffer  = std::make_unique<RingBuffer<std::shared_ptr<VideoFrame>>>(k_ring_capacity);
        unit.grabber = std::make_unique<VideoGrabber>(i, cam, *unit.buffer, d->triggerMgr);

        connect(unit.grabber.get(), &VideoGrabber::opened,
                this,               &VideoManager::camera_opened,   Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::closed,
                this,               &VideoManager::camera_closed,   Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::frame_dropped,
                this,               &VideoManager::frame_dropped,   Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::grab_error,
                this,               &VideoManager::camera_error,    Qt::QueuedConnection);

        connect(unit.grabber.get(), &VideoGrabber::preview_frame,
                this,               &VideoManager::frame_preview,   Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::calibration_frame_ready,
                this,               &VideoManager::calibration_frame_ready, Qt::QueuedConnection);
        connect(unit.grabber.get(), &VideoGrabber::action_command_capability,
                this,               &VideoManager::action_command_capability, Qt::QueuedConnection);

        connect(unit.grabber.get(), &VideoGrabber::frame_dropped, this,
                [this](int /*cam*/, int64_t /*id*/) {
                    d->totalDropped.fetch_add(1);
                }, Qt::QueuedConnection);

        log_info(QString("[VideoManager] open: calling open() for cam %1 (serial %2)")
                     .arg(i).arg(cam.serialNumber));
        if (unit.grabber->open()) {
            log_info(QString("[VideoManager] open: cam %1 open() returned, pushing unit").arg(i));
            d->units.push_back(std::move(unit));
            log_info(QString("[VideoManager] open: cam %1 unit pushed, opened=%2").arg(i).arg(opened + 1));
            ++opened;
        } else {
            log_warning(QString("[VideoManager] open: cam %1 open() failed").arg(i));
        }
    }

    d->cameraCount = opened;
    log_info(QString("[VideoManager] %1 of %2 camera(s) opened.")
                 .arg(opened).arg(settings.cameras.size()));
    return opened;
}

void VideoManager::close() {
    log_info(QString("[VideoManager] close() units=%1 rec=%2 prev=%3")
                 .arg(d->units.size()).arg(d->recording).arg(d->previewing));
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
        if (unit.grabber) { unit.grabber->close(); }
    }
    // Flush any queued MetaCallEvents (preview_frame, closed) that were posted
    // to this object by the now-stopped grabbers.  Removing them before
    // destroying the grabber objects prevents Qt from delivering events that
    // reference sender objects that are about to become dangling.
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    log_info("[VideoManager] close: clearing units");
    d->units.clear();
    d->cameraCount = 0;
    log_info("[VideoManager] close: done");
}

// ── Recording lifecycle ────────────────────────────────────────────────────

void VideoManager::start_preview() {
    if (d->units.empty()) { return; }
    arm_and_fire_action_commands();
    d->previewing = true;
    log_info(QString("[VideoManager] Preview started for %1 camera(s).").arg(d->units.size()));
}

void VideoManager::start(const QString&       sessionDir,
                          const QString&       videoBasename,
                          const VideoSettings& settings) {
    if (d->recording || d->units.empty()) { return; }

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
        const auto& cam = (i < static_cast<int>(settings.cameras.size()))
            ? settings.cameras[static_cast<size_t>(i)]
            : CameraParameters{};

        const QString suffix   = QString("_%1").arg(i);
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
        cfg.width         = cam.width;
        cfg.height        = cam.height;
        cfg.fps           = cam.fps;

        unit.encoder = std::make_unique<VideoEncoder>(cfg, *unit.buffer);

        connect(unit.encoder.get(), &VideoEncoder::encoding_stopped,
                this, &VideoManager::on_encoder_stopped, Qt::QueuedConnection);
        connect(unit.encoder.get(), &VideoEncoder::encoding_error,
                this, &VideoManager::camera_error,        Qt::QueuedConnection);

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
    arm_and_fire_action_commands();

    d->recording = true;
}

void VideoManager::stop() {
    if (!d->recording) { return; }
    d->recording = false;

    stop_action_ticker();

    // Signal grabbers to stop producing — encoders will drain and finish on their own.
    for (auto& unit : d->units) {
        if (unit.grabber) { unit.grabber->stop_grabbing(); }
        if (unit.encoder) { unit.encoder->stop_encoding(); }
    }
    // Wait for all threads to exit.
    for (auto& unit : d->units) {
        if (unit.grabber) { unit.grabber->wait(5000); }
        if (unit.encoder) { unit.encoder->wait(10000); }
    }
}

void VideoManager::apply_live_params(int configIndex) {
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
    // Identify every unit eligible to be freshly armed this call, and the
    // Action1-ready subset among them.
    std::vector<CameraUnit*> pending;       // all not-yet-running units
    std::vector<CameraUnit*> actionTargets; // subset that are Action1-ready
    for (auto& unit : d->units) {
        if (!unit.grabber || unit.grabber->isRunning()) { continue; } // already armed
        pending.push_back(&unit);
        if (unit.grabber->action_command_ready()) {
            actionTargets.push_back(&unit);
        }
    }

    // Phase 1 (arm): start every pending grabber's thread. Cameras
    // configured for Action1 are now parked waiting for FrameStart
    // triggers (see VideoGrabber::open()) — nothing arrives until the
    // ticker below starts.
    for (auto* u : pending) { u->grabber->start_grabbing(); }
    if (actionTargets.empty()) { return; }

    // start_grabbing() only confirms QThread::start() was scheduled, not
    // that Pylon's StartGrabbing() has actually run yet (that call itself
    // does real node-map I/O first) — firing immediately risks the ticker's
    // first tick or two reaching a camera before it's truly listening,
    // silently dropping that camera's earliest trigger(s) and skewing its
    // first frame relative to the rest of the group. Poll briefly, bounded
    // so a slow/stuck camera can never turn this into an indefinite wait —
    // matches this feature's established "degrade safely, never hang"
    // philosophy; a camera that's still not ready when the timeout expires
    // just gets whatever tick catches it next, self-correcting rather than
    // held up entirely.
    {
        constexpr int k_readinessPollTimeoutMs = 500;
        constexpr int k_readinessPollIntervalMs = 2;
        int waitedMs = 0;
        while (waitedMs < k_readinessPollTimeoutMs) {
            const bool allReady = std::all_of(actionTargets.begin(), actionTargets.end(),
                [](CameraUnit* u) { return u->grabber->is_actually_grabbing(); });
            if (allReady) { break; }
            QThread::msleep(k_readinessPollIntervalMs);
            waitedMs += k_readinessPollIntervalMs;
        }
        if (waitedMs >= k_readinessPollTimeoutMs) {
            log_warning("[VideoManager] Timed out waiting for all Action1-armed cameras to "
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
    std::vector<VideoGrabber*>       grabbers; // parallel to targets — see ActionCommandTicker
    std::vector<double>              targetFps;
    QStringList                      targetDesc;
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
        const QString msg = "GigE transport layer unavailable — Action Command triggering "
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
                 .arg(periodMs, 0, 'f', 1).arg(targets.size()).arg(targetDesc.join(", ")));
    d->actionTicker = std::make_unique<ActionCommandTicker>(std::move(session), std::move(targets),
                                                             std::move(grabbers), periodMs);
    d->actionTicker->start();
}

void VideoManager::stop_action_ticker() {
    if (!d->actionTicker) { return; }
    d->actionTicker->requestInterruption();
    if (!d->actionTicker->wait(5000)) {
        log_warning("[VideoManager] ActionCommandTicker did not finish in 5 s — forcing terminate");
        d->actionTicker->terminate();
        d->actionTicker->wait();
    }
    d->actionTicker.reset();
}

void VideoManager::on_encoder_stopped(int cameraIndex, int64_t frames) {
    d->totalEncoded.fetch_add(frames);
    log_info(QString("[VideoManager] Camera %1 encoder done (%2 frames).")
                 .arg(cameraIndex).arg(frames));
    const int done = d->stoppedCount.fetch_add(1) + 1;
    if (done >= d->cameraCount) {
        emit recording_stopped();
    }
}

// ── Accessors ──────────────────────────────────────────────────────────────

bool    VideoManager::is_recording()        const { return d->recording; }
bool    VideoManager::is_previewing()       const { return d->previewing; }
int     VideoManager::camera_count()        const { return d->cameraCount; }
int64_t VideoManager::total_frames_encoded() const { return d->totalEncoded.load(); }
int64_t VideoManager::total_frames_dropped() const { return d->totalDropped.load(); }

VideoManager::CameraStats VideoManager::camera_stats(int index) const {
    CameraStats stats;
    if (index < 0 || index >= static_cast<int>(d->units.size())) { return stats; }
    const auto& unit = d->units[static_cast<size_t>(index)];
    if (unit.grabber) {
        stats.fps                = unit.grabber->current_fps();
        stats.framesGrabbed      = unit.grabber->frames_grabbed();
        stats.framesDropped      = unit.grabber->frames_dropped();
        stats.grabberRunning     = unit.grabber->isRunning();
        stats.lastFrameElapsedNs = unit.grabber->last_frame_elapsed_ns();
    }
    if (unit.encoder) {
        stats.framesEncoded = unit.encoder->frames_encoded();
    }
    if (unit.buffer) {
        const auto avail = unit.buffer->available_read();
        const auto cap   = unit.buffer->capacity();
        stats.ringFillPct = (cap > 0)
            ? static_cast<int>((avail * 100ULL) / cap)
            : 0;
    }
    return stats;
}

} // namespace mosaic
