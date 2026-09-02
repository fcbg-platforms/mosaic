#include "ui/monitor_bridge.hpp"

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
