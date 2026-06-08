#include "ui/monitor_bridge.hpp"
#include "utils/logger.hpp"

namespace mosaic {

MonitorBridge::MonitorBridge(RecordManager*       recordMgr,
                              const VideoSettings& videoSettings,
                              QObject*             parent)
    : QObject(parent), m_rm(recordMgr), m_videoSettings(videoSettings),
      m_cameraCount(static_cast<int>(videoSettings.cameras.size()))
{
    // Relay RecordManager signals → QML property notifications
    connect(m_rm, &RecordManager::recording_started, this,
            [this](const QString& path) {
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
            [this](int /*ms*/) {
        emit elapsedMsChanged();
    });

    connect(m_rm, &RecordManager::error_occurred, this,
            [](const QString& msg) {
        log_error(QString("[RecordManager] %1").arg(msg));
    });
}

// ── Q_PROPERTY readers ─────────────────────────────────────────────────────

bool    MonitorBridge::isRecording() const { return m_rm->is_recording(); }
int     MonitorBridge::elapsedMs()   const { return m_rm->elapsed_ms();   }
int     MonitorBridge::cameraCount() const { return m_cameraCount;         }
QString MonitorBridge::sessionPath() const { return m_sessionPath;         }

// ── Q_INVOKABLEs ───────────────────────────────────────────────────────────

void MonitorBridge::startRecording() {
    if (!m_rm->start())
        log_warning("Recording could not start — check the log for details.");
}
void MonitorBridge::stopRecording() { m_rm->stop(); }

// ── Camera count update ────────────────────────────────────────────────────

void MonitorBridge::set_camera_count(int count) {
    if (m_cameraCount == count) return;
    m_cameraCount = count;
    emit cameraCountChanged();
}

} // namespace mosaic
