#include "ui/monitor_bridge.hpp"

#include <QDateTime>
#include <QFile>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

#include "utils/logger.hpp"
#include "video/camera_label.hpp"

namespace mosaic {

MonitorBridge::MonitorBridge(RecordManager* recordMgr, const VideoSettings& videoSettings,
                             const RecordSettings& recordSettings, QObject* parent)
    : QObject(parent),
      m_rm(recordMgr),
      m_videoSettings(videoSettings),
      m_recordSettings(recordSettings),
      m_cameraCount(static_cast<int>(videoSettings.cameras.size())),
      m_hidePreviews(recordSettings.hidePreviewsWhileRecording) {
    m_frameGens.resize(m_cameraCount, 0);
    refresh_video_settings();

    // Prefill from the last recording. Running one participant through several
    // tasks is the normal case, so retyping the subject every run is friction
    // — and a field left blank because it was tedious is how a session ends up
    // unattributable. Notes are deliberately not restored: they describe one
    // recording.
    m_subjectLabel = recordSettings.lastIdentity.subject;
    m_sessionLabel = recordSettings.lastIdentity.session;
    m_taskLabel    = recordSettings.lastIdentity.task;
    publish_identity(); // arm the prefill; a trigger may fire before any edit

    // Notes stay editable during a recording, so they need flushing without
    // writing on every keystroke. One idle-triggered write costs at most a few
    // seconds of typing in a crash, which is the right trade for not hammering
    // the disk that is simultaneously taking six camera streams.
    m_notesFlushTimer = new QTimer(this);
    m_notesFlushTimer->setSingleShot(true);
    m_notesFlushTimer->setInterval(5000);
    connect(m_notesFlushTimer, &QTimer::timeout, this, [this] { flush_notes_to_disk(); });

    // Repeating 1 s ticker driving the pre-recording countdown. Armed by
    // startRecording(), stopped by cancel_countdown()/begin_recording_now().
    m_countdownTimer = new QTimer(this);
    m_countdownTimer->setInterval(1000);
    connect(m_countdownTimer, &QTimer::timeout, this, [this] {
        if (m_countdownSeconds <= 0) { // defensive: nothing pending
            m_countdownTimer->stop();
            return;
        }
        --m_countdownSeconds;
        emit countdownSecondsChanged();
        if (m_countdownSeconds == 0) {
            m_countdownTimer->stop();
            begin_recording_now();
        }
    });

    connect(m_rm, &RecordManager::recording_started, this, [this](const QString& path) {
        // A recording can also be started straight through RecordManager by a
        // StartRecording trigger (see Application's action_requested handler),
        // which knows nothing about this countdown. If that happens mid-count,
        // the pending start is moot — drop it rather than let it fire into an
        // already-running session.
        cancel_countdown();
        set_start_pending(false);
        m_sessionPath = path;
        emit recordingChanged();
        emit sessionPathChanged();
    });

    connect(m_rm, &RecordManager::recording_stopped, this,
            [this](const QString& /*path*/, int /*durationMs*/) {
                // Last chance: a note typed in the final seconds would
                // otherwise die with the pending debounce.
                m_notesFlushTimer->stop();
                flush_notes_to_disk();
                // Then clear it. The box belongs to the *next* recording from
                // here on, and leaving the text in place would silently copy
                // one session's note into the following session's notes.txt —
                // the note would look right on screen and be attached to the
                // wrong recording. Editing the note of the session that just
                // finished happens in the Session Health dialog that opens on
                // stop, or later in the Session Browser.
                if (!m_notes.isEmpty()) {
                    m_notes.clear();
                    publish_identity();
                    emit notesChanged();
                }
                emit recordingChanged();
                emit elapsedMsChanged();
            });

    connect(m_rm, &RecordManager::elapsed_ms_changed, this,
            [this](int /*ms*/) { emit elapsedMsChanged(); });

    connect(m_rm, &RecordManager::error_occurred, this,
            [](const QString& msg) { log_error(QString("[RecordManager] %1").arg(msg)); });
}

// ── Q_PROPERTY readers ─────────────────────────────────────────────────────

bool MonitorBridge::isRecording() const { return m_rm->is_recording(); }
int MonitorBridge::elapsedMs() const { return m_rm->elapsed_ms(); }
int MonitorBridge::cameraCount() const { return m_cameraCount; }
QString MonitorBridge::sessionPath() const { return m_sessionPath; }
int MonitorBridge::frameGen() const { return m_frameGen; }
QVariantList MonitorBridge::frameGens() const { return m_frameGens; }
QVariantList MonitorBridge::cameraHealth() const { return m_cameraHealth; }
int MonitorBridge::countdownSeconds() const { return m_countdownSeconds; }
bool MonitorBridge::startPending() const { return m_startPending; }
bool MonitorBridge::hidePreviews() const { return m_hidePreviews; }
bool MonitorBridge::interviewMode() const { return m_interviewMode; }
int MonitorBridge::interviewCameraIndex() const { return m_interviewCameraIndex; }
double MonitorBridge::interviewFps() const { return m_interviewFps; }
bool MonitorBridge::interviewSwitching() const { return m_interviewSwitching; }
double MonitorBridge::interviewCameraMaxFps() const { return m_interviewCameraMaxFps; }
double MonitorBridge::interviewMeasuredFps() const { return m_interviewMeasuredFps; }

// ── Video settings mirror ──────────────────────────────────────────────────

void MonitorBridge::refresh_video_settings() {
    m_interviewMode        = m_videoSettings.interview_active();
    m_interviewCameraIndex = m_videoSettings.interview.cameraIndex;
    m_interviewFps         = m_videoSettings.interview.fps;
    emit interviewChanged();
}

void MonitorBridge::set_interview_rates(double cameraMaxFps, double measuredFps) {
    // Rounded to what the badge shows, so a reading that jitters in the third
    // decimal does not repaint the header every couple of seconds.
    const double maxR  = cameraMaxFps > 0 ? std::round(cameraMaxFps * 10.0) / 10.0 : -1.0;
    const double measR = measuredFps > 0 ? std::round(measuredFps * 10.0) / 10.0 : -1.0;
    if (maxR == m_interviewCameraMaxFps && measR == m_interviewMeasuredFps) return;
    m_interviewCameraMaxFps = maxR;
    m_interviewMeasuredFps  = measR;
    emit interviewChanged();
}

void MonitorBridge::set_interview_switching(bool switching) {
    if (switching == m_interviewSwitching) return;
    m_interviewSwitching = switching;
    emit interviewChanged();
}

void MonitorBridge::requestInterviewMode(bool on) {
    if (m_interviewSwitching || on == m_interviewMode) return;
    emit interviewModeRequested(on);
}

// ── Camera health ──────────────────────────────────────────────────────────

void MonitorBridge::update_camera_health(const std::vector<double>& fps,
                                         const std::vector<bool>& running) {
    // Each camera is judged against the *others*, not a single shared median —
    // see classify_cameras() for why that distinction is not cosmetic.
    const std::vector<CameraHealth> states = classify_cameras(fps, running);

    // Transitions worth a log line, only while recording: in preview the chips
    // are the record, and a cable being reseated would otherwise fill the log.
    const bool recording = m_rm && m_rm->is_recording();
    // At the first tick of a recording, compare against "all fine" rather than
    // against preview: a camera that was already behind when Record was
    // clicked never *changes* state, and would otherwise go unlogged for the
    // whole session — the case the log line exists for.
    const bool recordingJustStarted = recording && !m_healthRecording;
    m_healthRecording               = recording;
    if (recording) {
        for (std::size_t i = 0; i < states.size(); ++i) {
            const CameraHealth before = recordingJustStarted      ? CameraHealth::Ok
                                        : i < m_lastHealth.size() ? m_lastHealth[i]
                                                                  : CameraHealth::Unknown;
            const CameraHealth now    = states[i];
            if (now == before) continue;
            const QString name = camera_label(static_cast<int>(i));
            if (now == CameraHealth::Stalled || now == CameraHealth::Lagging) {
                // The baseline the verdict used: the *other* running cameras.
                std::vector<double> peers;
                for (std::size_t j = 0; j < fps.size(); ++j) {
                    if (j != i && j < running.size() && running[j]) peers.push_back(fps[j]);
                }
                const double median = peer_median_fps(std::move(peers));
                log_warning(QString("[Health] %1 is %2 (%3 fps; the other cameras ~%4 fps).")
                                .arg(name)
                                .arg(now == CameraHealth::Stalled
                                         ? "delivering far fewer frames than the others"
                                         : "falling behind the other cameras")
                                .arg(fps[i], 0, 'f', 1)
                                .arg(median, 0, 'f', 1));
            } else if (before == CameraHealth::Stalled || before == CameraHealth::Lagging) {
                if (now == CameraHealth::Ok) {
                    log_info(QString("[Health] %1 is keeping up again (%2 fps).")
                                 .arg(name)
                                 .arg(fps[i], 0, 'f', 1));
                } else {
                    // Unknown: either its grab thread stopped — worse than
                    // stalled, and the chip disappearing must not be the only
                    // trace of it — or no other camera is running to compare
                    // it with.
                    const bool selfRunning = i < running.size() && running[i];
                    log_warning(selfRunning
                                    ? QString("[Health] %1 can no longer be compared: no other "
                                              "camera is running.")
                                          .arg(name)
                                    : QString("[Health] %1 has stopped grabbing.").arg(name));
                }
            }
        }
    }
    m_lastHealth = states;

    QVariantList out;
    out.reserve(static_cast<qsizetype>(fps.size()));
    for (std::size_t i = 0; i < fps.size(); ++i) {
        QVariantMap entry;
        // Rounded to the one decimal the chip renders. Not cosmetic: the raw
        // rate jitters every second, so comparing unrounded doubles below would
        // differ on almost every tick and the republish guard would never fire.
        entry["fps"]   = std::round(fps[i] * 10.0) / 10.0;
        entry["state"] = static_cast<int>(states[i]);
        out.append(entry);
    }

    // Only republish on a visible change. This runs once a second for the life
    // of the app, and a QVariantList property change re-evaluates every binding
    // that reads it.
    if (out == m_cameraHealth) {
        return;
    }
    m_cameraHealth = out;
    emit cameraHealthChanged();
}

// ── Record settings mirror ─────────────────────────────────────────────────

void MonitorBridge::refresh_record_settings() {
    // Before the early return below, because most of what this signal reports
    // has nothing to do with previews: the recordings directory, "add
    // timestamp", the timestamp format. All three change what the folder
    // preview says, and the too-long warning now quotes the directory by name —
    // so an operator who reacts to "too long for D:/very/long/path" by pointing
    // Record settings somewhere shorter would otherwise keep reading the old
    // path until they happened to touch a label field. Click-time behaviour was
    // always correct (startRecording() recomputes first); this is about not
    // showing them something false in the meantime.
    recompute_identity_preview();
    emit identityChanged();

    const bool hide = m_recordSettings.hidePreviewsWhileRecording;
    if (hide == m_hidePreviews) return;
    m_hidePreviews = hide;
    emit hidePreviewsChanged();
}

// ── Q_INVOKABLEs ───────────────────────────────────────────────────────────

void MonitorBridge::startRecording() {
    // Idempotent: a second click, or Ctrl+R while the countdown is already
    // running, must not re-arm it or start a second recording.
    if (m_rm->is_recording() || m_countdownSeconds > 0) return;
    // A confirmation dialog is already on screen; QMessageBox/QDialog::exec()
    // spins the event loop, so clicks still arrive here.
    if (m_awaitingConfirmation) return;

    recompute_identity_preview();

    // The rig check — cameras, sync, microphones, disk. Run now, at the click,
    // so it describes the rig as it is when recording is about to begin.
    m_preflight = (m_recordSettings.runPreflightChecks && m_preflightProvider)
                      ? m_preflightProvider()
                      : PreflightReport{};

    // Ask before doing anything, never after a 3-2-1 countdown. One dialog
    // covers every reason to ask — the identity is unusable (no subject, or a
    // name too long for the recordings directory), this combination has been
    // recorded before, or a pre-flight check found something. Merged
    // deliberately: they can hold at once, and two modals in a row is how a
    // confirmation stops being read.
    if (!m_advice.canRecord || m_advice.collision.collides() || m_preflight.needs_attention()) {
        m_awaitingConfirmation = true;
        emit identityConfirmationNeeded(m_subjectLabel, m_sessionLabel, m_taskLabel);
        return; // deliberately NOT armed yet
    }

    arm_countdown(current_identity());
}

bool MonitorBridge::confirmIdentityAndStart(const QString& subject, const QString& session,
                                            const QString& task) {
    m_awaitingConfirmation = false;

    // Three strings, not a SessionIdentity. current_identity() also carries the
    // operator's notes (see it), so accepting a whole identity built from the
    // dialog's three fields would silently wipe whatever was typed in the notes
    // box — the one thing on that screen there is no way to get back.
    // Written before the is_recording() check below, not after: if a trigger
    // did race us, throwing away what the operator just typed would be the
    // wrong repair — those labels are still the ones they want for the next
    // recording. Safe against the running session, because
    // RecordManager::set_session_identity() refuses to change an identity once
    // that session's folder exists.
    m_subjectLabel = subject;
    m_sessionLabel = session;
    m_taskLabel    = task;
    publish_identity();
    emit identityChanged(); // the inline bar mirrors what the dialog was told

    // A trigger can start a recording while the dialog is open: exec() spins a
    // nested event loop, and TriggerManager's signal is not user input, so
    // modality does not hold it back. Re-checked here rather than trusted from
    // startRecording() for that reason — and said out loud, because an accepted
    // dialog that silently records nothing is indistinguishable from a bug.
    if (m_rm->is_recording()) {
        log_warning(
            "[MonitorBridge] A recording was already started (by a trigger) while the "
            "session details were being entered — the details were saved but no second "
            "recording was begun.");
        return false;
    }
    arm_countdown(current_identity());
    return true;
}

void MonitorBridge::set_preflight_provider(PreflightProvider provider) {
    m_preflightProvider = std::move(provider);
}

const PreflightReport& MonitorBridge::last_preflight() const { return m_preflight; }

void MonitorBridge::cancelPendingStart() {
    m_awaitingConfirmation = false;
    log_info("[MonitorBridge] Session-details prompt dismissed — nothing was recorded.");
}

void MonitorBridge::arm_countdown(const SessionIdentity& id) {
    // The guard lives here, not only in the callers. Every path that starts a
    // recording funnels through this function, and re-deriving the condition at
    // each one is how a new caller silently gets a second countdown stacked on
    // a running session.
    if (m_rm->is_recording() || m_countdownSeconds > 0) return;

    m_rm->set_session_identity(id);
    // Reaching here past a dialog that listed problems means "Record anyway":
    // keep a record of what was overridden. Empty when nothing was.
    m_rm->set_preflight_warnings(m_preflight.problems());

    const int configured = m_recordSettings.startDelaySec;
    const int delay      = std::clamp(configured, 0, RecordSettings::kMaxStartDelaySec);
    if (delay != configured) {
        log_warning(QString("[MonitorBridge] Start delay %1 s is out of range — using %2 s.")
                        .arg(configured)
                        .arg(delay));
    }
    set_start_pending(true);
    if (delay == 0) {
        begin_recording_now();
        return;
    }

    m_countdownSeconds = delay;
    emit countdownSecondsChanged();
    m_countdownTimer->start();
    log_info(QString("[MonitorBridge] Recording starts in %1 s.").arg(delay));
}

void MonitorBridge::start_from_trigger() {
    // RecordManager::start() has its own already-recording guard, but reaching
    // it would log "start() called while already recording" — which reads as a
    // fault when it is the ordinary case of two triggers arriving close
    // together.
    if (m_rm->is_recording()) return;

    const SessionIdentity id = current_identity();
    if (!names_a_subject(id)) {
        // Not a refusal. Unlike the Record button there is nobody here to
        // correct it, and a recording that happened under an awkward name can
        // be renamed afterwards while one that never happened cannot.
        log_warning(
            "[MonitorBridge] Trigger started a recording with no subject set — it will "
            "be named by timestamp only and cannot be attributed to a participant "
            "afterwards.");
    }

    // No duplicate-name scan here on purpose. It costs a synchronous directory
    // listing on the thread that also drives the camera previews, and it would
    // only be advisory: RecordManager::start() resolves the run index
    // authoritatively for both callers, and the path budget is enforced there
    // too. A trigger's start latency is the experiment's, so it buys nothing
    // worth the delay.
    m_rm->set_session_identity(id);
    // A trigger runs no checks, so it must not carry findings from an
    // earlier, abandoned click.
    m_rm->set_preflight_warnings({});
    set_start_pending(true);
    // begin_recording_now(), not arm_countdown(): the start delay exists so a
    // human who clicked can get out of shot. A trigger fires at the moment the
    // experiment means, and silently shifting that by three seconds would
    // corrupt exactly the alignment triggers exist to provide. It also clears
    // any countdown already running, so an operator's pending click cannot
    // fire a second session in on top of this one.
    begin_recording_now();
}

void MonitorBridge::stopRecording() {
    if (m_countdownSeconds > 0) {
        cancel_countdown();
        log_info("[MonitorBridge] Start countdown cancelled — nothing was recorded.");
        return;
    }
    m_rm->stop();
}

// ── Countdown helpers ──────────────────────────────────────────────────────

void MonitorBridge::begin_recording_now() {
    // Clear the countdown *before* start(), so a failed start can't leave the
    // UI stuck showing a countdown that will never resolve. startPending stays
    // true across the call — see its Q_PROPERTY doc comment for why.
    m_countdownTimer->stop();
    if (m_countdownSeconds != 0) {
        m_countdownSeconds = 0;
        emit countdownSecondsChanged();
    }

    // A StartRecording trigger may have started a session while we were
    // counting down. Calling start() again would just hit RecordManager's own
    // already-recording guard and log "Recording could not start", which would
    // be actively misleading — a recording *is* running, just not this one.
    if (m_rm->is_recording()) {
        log_info(
            "[MonitorBridge] Countdown elapsed but a recording is already "
            "running — pending start dropped.");
        set_start_pending(false);
        return;
    }

    if (!m_rm->start()) log_warning("Recording could not start — check the log for details.");
    // On success RecordManager::recording_started has already fired and
    // cleared this; on failure it's what releases the previews again.
    set_start_pending(false);
}

void MonitorBridge::cancel_countdown() {
    m_countdownTimer->stop();
    // Cleared before the early return below, and this is reachable: while the
    // session-details dialog is open there is no countdown, but QDialog::exec()
    // spins the event loop, so a StopRecording *trigger* can still land
    // (Application's action_requested handler ->
    // MainWindow::cancel_pending_recording_start() -> here). Leaving the flag
    // set on that path would make the bridge permanently deaf to Record.
    //
    // Belt and braces now rather than the sole guarantee: MainWindow's exec()
    // is stack-scoped and always follows with exactly one of
    // confirmIdentityAndStart() or cancelPendingStart(), so the flag clears on
    // every path anyway. Kept because the cost is one assignment and the
    // failure it prevents is a Record button that stays dead for the rest of
    // the session.
    //
    // Note what this deliberately does *not* do: a stop trigger arriving while
    // the operator is mid-sentence in the dialog does not veto the Start they
    // press a moment later. They are answering a question that was asked before
    // the trigger fired, and an explicit click is the more recent instruction.
    m_awaitingConfirmation = false;
    if (m_countdownSeconds == 0) return;
    m_countdownSeconds = 0;
    emit countdownSecondsChanged();
    set_start_pending(false);
}

void MonitorBridge::set_start_pending(bool pending) {
    if (m_startPending == pending) return;
    m_startPending = pending;
    emit startPendingChanged();
}

// ── Session identity ───────────────────────────────────────────────────────

QString MonitorBridge::subjectLabel() const { return m_subjectLabel; }
QString MonitorBridge::sessionLabel() const { return m_sessionLabel; }
QString MonitorBridge::taskLabel() const { return m_taskLabel; }
QString MonitorBridge::notes() const { return m_notes; }

SessionIdentity MonitorBridge::current_identity() const {
    SessionIdentity id;
    id.subject = m_subjectLabel;
    id.session = m_sessionLabel;
    id.task    = m_taskLabel;
    id.notes   = m_notes;
    return id;
}

void MonitorBridge::setSubjectLabel(const QString& v) {
    if (m_subjectLabel == v) return;
    m_subjectLabel = v;
    publish_identity();
    emit identityChanged();
}

void MonitorBridge::setSessionLabel(const QString& v) {
    if (m_sessionLabel == v) return;
    m_sessionLabel = v;
    publish_identity();
    emit identityChanged();
}

void MonitorBridge::setTaskLabel(const QString& v) {
    if (m_taskLabel == v) return;
    m_taskLabel = v;
    publish_identity();
    emit identityChanged();
}

void MonitorBridge::setNotes(const QString& v) {
    if (m_notes == v) return;
    m_notes = v;
    publish_identity();
    emit notesChanged();
    // Only meaningful mid-recording; before one starts, the text is carried
    // into the session by RecordManager::start() instead.
    if (m_rm->is_recording()) {
        m_notesFlushTimer->start();
    }
}

void MonitorBridge::clearIdentity() {
    m_subjectLabel.clear();
    m_sessionLabel.clear();
    m_taskLabel.clear();
    m_notes.clear();
    publish_identity();
    emit identityChanged();
    emit notesChanged();
}

void MonitorBridge::publish_identity() {
    // Pushed on every edit rather than only when Record is clicked, because
    // start() has a second caller that never comes through here: a
    // StartRecording trigger goes straight to RecordManager. Without this, a
    // trigger-started session would be written with an empty identity while
    // the operator was looking at a filled-in form — silently losing exactly
    // the attribution this feature exists to capture.
    m_rm->set_session_identity(current_identity());
    recompute_identity_preview();
}

QString MonitorBridge::folderPreview() const {
    // advise_identity() returns an empty name when there is nothing recordable,
    // rather than the name build_session_folder_name() would have produced —
    // showing an accurate preview of a folder the operator cannot create is
    // worse than showing none. The placeholder is a UI affordance, so it is
    // worded here; QML prefixes this with "▸ " unconditionally and would
    // otherwise render a bare arrow pointing at nothing.
    if (m_advice.folderName.isEmpty()) {
        return QStringLiteral("(enter a subject to name this session)");
    }
    return m_advice.folderName;
}

QString MonitorBridge::identityWarning() const { return m_advice.warning; }

void MonitorBridge::recompute_identity_preview() {
    // One directory listing, one pass, one set of answers. All of the judgement
    // lives in advise_identity() rather than here: the dialog that opens on
    // Record needs exactly the same answers about exactly the same three
    // labels, and anything decided in this class cannot be unit-tested at all
    // (mosaic_tests links Qt6::Core and Qt6::Network; this header pulls QtGui
    // in via QImage).
    //
    // Not cached. It is one QDir::entryList per keystroke, which is nothing for
    // the tens-to-hundreds of sessions a recordings directory actually holds —
    // and it used to be three, because check_collision() re-derived the sibling
    // list that next_run_index() had already built. If `directory` is ever
    // pointed at a slow network share this is the first thing to revisit; a
    // cache would be safe to add because RecordManager::start() re-resolves the
    // run index and re-checks the budget authoritatively, so a stale reading
    // here could only ever make the *preview* wrong, never the folder created.
    m_advice = advise_identity(
        current_identity(), existing_session_names(m_recordSettings.directory),
        m_recordSettings.directory, QDateTime::currentDateTime(),
        m_recordSettings.addTimestamp ? m_recordSettings.timestampFormat : QString());
}

void MonitorBridge::flush_notes_to_disk() const {
    const QString sessionPath = m_rm->current_session_path();
    if (sessionPath.isEmpty()) return;
    const QString trimmed = m_notes.trimmed();
    QFile f(sessionPath + "/notes.txt");
    if (trimmed.isEmpty()) {
        // Emptied on purpose — remove the file rather than leave a stale note.
        f.remove();
        return;
    }
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(trimmed.toUtf8());
    }
}

// ── Camera count ───────────────────────────────────────────────────────────

void MonitorBridge::set_camera_count(int count) {
    if (m_cameraCount == count) return;
    m_cameraCount = count;
    m_frameGens.resize(count, 0);
    // Rebuilt now rather than at the next poll a second from now: a removed
    // camera must not keep a chip reporting its old rate, and a new one must
    // not leave a hole. Real "unknown" entries, so every element is a map with
    // the two keys QML reads.
    QVariantList health;
    health.reserve(count);
    for (int i = 0; i < count; ++i) {
        health.append(
            QVariantMap{{"fps", 0.0}, {"state", static_cast<int>(CameraHealth::Unknown)}});
    }
    m_cameraHealth = health;
    m_lastHealth.assign(static_cast<std::size_t>(std::max(0, count)), CameraHealth::Unknown);
    if (m_feedProvider) m_feedProvider->set_camera_count(count);
    emit cameraCountChanged();
    emit frameGensChanged();
    emit cameraHealthChanged();
}

void MonitorBridge::set_feed_provider(VideoFeedProvider* provider) {
    m_feedProvider = provider;
    if (m_feedProvider) m_feedProvider->set_camera_count(m_cameraCount);
}

// ── Frame preview ───────────────────────────────────────────────────────────

void MonitorBridge::on_frame_preview(int cameraIndex, QImage frame) {
    if (!m_feedProvider) return;
    m_feedProvider->update_frame(cameraIndex, frame);

    // Grow per-camera list if needed (handles cameras that open after construction).
    if (cameraIndex >= m_frameGens.size()) m_frameGens.resize(cameraIndex + 1, 0);
    m_frameGens[cameraIndex] = m_frameGens[cameraIndex].toInt() + 1;

    ++m_frameGen;
    emit frameGensChanged(); // per-camera first so QML reads the updated list
    emit frameGenChanged();
}

} // namespace mosaic
