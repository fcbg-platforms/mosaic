#include "ui/monitor_bridge.hpp"

#include <QDateTime>
#include <QFile>
#include <algorithm>

#include "utils/logger.hpp"

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
int MonitorBridge::countdownSeconds() const { return m_countdownSeconds; }
bool MonitorBridge::startPending() const { return m_startPending; }
bool MonitorBridge::hidePreviews() const { return m_hidePreviews; }

// ── Record settings mirror ─────────────────────────────────────────────────

void MonitorBridge::refresh_record_settings() {
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
    // A duplicate-name question is already on screen; QMessageBox::exec()
    // spins the event loop, so clicks still arrive here.
    if (m_awaitingCollisionAnswer) return;

    const SessionIdentity id = current_identity();

    // Refuse a name that would leave no room for the files written *inside*
    // the session. Measured against m_folderPreview, which has the run index
    // resolved: building the name from `id` here instead would omit "_run-NN"
    // and let a name through that the warning line right next to the button
    // was already calling too long.
    recompute_identity_preview();
    if (!fits_path_budget(m_recordSettings.directory, m_folderPreview)) {
        log_warning(
            "[MonitorBridge] Session name is too long for the recordings "
            "directory — shorten a label before recording.");
        emit identityChanged(); // republishes identityWarning
        return;
    }

    // Ask before doing anything, never after a 3-2-1 countdown.
    const auto report = check_collision(existing_session_names(m_recordSettings.directory), id);
    if (report.collides()) {
        m_awaitingCollisionAnswer = true;
        m_collisionIdentity       = id;
        emit runCollisionDetected(entity_prefix(id), report.existingCount, report.suggestedRun);
        return; // deliberately NOT armed yet
    }

    arm_countdown(id);
}

void MonitorBridge::confirmRunAndStart(int run) {
    Q_UNUSED(run); // start() re-resolves the index authoritatively
    m_awaitingCollisionAnswer = false;
    // A StartRecording trigger can fire while the modal is open, because
    // exec() runs a nested event loop. Re-check rather than starting a second
    // session on top of it.
    if (m_rm->is_recording() || m_countdownSeconds > 0) return;
    arm_countdown(m_collisionIdentity);
}

void MonitorBridge::cancelPendingStart() {
    m_awaitingCollisionAnswer = false;
    log_info("[MonitorBridge] Duplicate-name prompt dismissed — nothing was recorded.");
}

void MonitorBridge::arm_countdown(const SessionIdentity& id) {
    m_rm->set_session_identity(id);

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
    // duplicate-name prompt is open there is no countdown, but QMessageBox::
    // exec() spins the event loop, so a StopRecording *trigger* can still land
    // (Application's action_requested handler ->
    // MainWindow::cancel_pending_recording_start() -> here). Leaving the flag
    // set on that path would make the bridge permanently deaf to Record.
    m_awaitingCollisionAnswer = false;
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

QString MonitorBridge::folderPreview() const { return m_folderPreview; }

QString MonitorBridge::identityWarning() const { return m_identityWarning; }

void MonitorBridge::recompute_identity_preview() {
    m_resolvedIdentity = current_identity();
    if (m_resolvedIdentity.has_entities()) {
        // One listing, reused for the preview, the warning and the pre-flight
        // length check below.
        m_resolvedIdentity.run =
            next_run_index(existing_session_names(m_recordSettings.directory), m_resolvedIdentity);
    }
    m_folderPreview = build_session_folder_name(
        m_resolvedIdentity, QDateTime::currentDateTime(),
        m_recordSettings.addTimestamp ? m_recordSettings.timestampFormat : QString());

    m_identityWarning = QString();

    // Report the first problem only. A field-by-field breakdown is noise
    // beside a preview that already shows the result.
    struct Field {
        const char* name;
        const QString& raw;
    };
    for (const Field f : {Field{"Subject", m_subjectLabel}, Field{"Session", m_sessionLabel},
                          Field{"Task", m_taskLabel}}) {
        if (f.raw.isEmpty()) continue;
        const QString clean = sanitize_label(f.raw);
        if (clean == f.raw) continue;

        if (clean.isEmpty()) {
            m_identityWarning = QString("%1 \"%2\" has no letters or digits and will be ignored.")
                                    .arg(f.name, f.raw);
        } else if (sanitize_label(f.raw, f.raw.size()) == f.raw) {
            // Nothing was dropped for being the wrong kind of character — the
            // label is simply too long. Saying "only letters and digits are
            // allowed" here would be actively wrong, and would hide the real
            // danger: two long ids sharing a prefix collapse to one identity.
            m_identityWarning = QString(
                                    "%1 is longer than %2 characters and will be shortened "
                                    "to \"%3\" — check it still identifies uniquely.")
                                    .arg(f.name)
                                    .arg(k_max_label_chars)
                                    .arg(clean);
        } else {
            m_identityWarning =
                QString("%1 will be recorded as \"%2\" — only letters and digits are allowed.")
                    .arg(f.name, clean);
        }
        break;
    }

    if (m_identityWarning.isEmpty() &&
        !fits_path_budget(m_recordSettings.directory, m_folderPreview)) {
        m_identityWarning = QString("This name is too long for %1 — shorten a label.")
                                .arg(m_recordSettings.directory);
    }
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
    if (m_feedProvider) m_feedProvider->set_camera_count(count);
    emit cameraCountChanged();
    emit frameGensChanged();
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
