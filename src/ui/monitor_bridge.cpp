#include "ui/monitor_bridge.hpp"
#include "utils/logger.hpp"

namespace mosaic {

MonitorBridge::MonitorBridge(RecordManager*       recordMgr,
                              const VideoSettings& videoSettings,
                              QObject*             parent)
    : QObject(parent), m_rm(recordMgr), m_videoSettings(videoSettings),
      m_cameraCount(static_cast<int>(videoSettings.cameras.size()))
{
    m_frameGens.resize(m_cameraCount, 0);

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

bool         MonitorBridge::isRecording()      const { return m_rm->is_recording(); }
int          MonitorBridge::elapsedMs()        const { return m_rm->elapsed_ms();   }
int          MonitorBridge::cameraCount()      const { return m_cameraCount;         }
QString      MonitorBridge::sessionPath()      const { return m_sessionPath;         }
int          MonitorBridge::frameGen()         const { return m_frameGen;            }
QVariantList MonitorBridge::frameGens()        const { return m_frameGens;           }

// ── Q_INVOKABLEs ───────────────────────────────────────────────────────────

void MonitorBridge::startRecording() {
    if (!m_rm->start())
        log_warning("Recording could not start — check the log for details.");
}
void MonitorBridge::stopRecording() { m_rm->stop(); }

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
    if (cameraIndex >= m_frameGens.size())
        m_frameGens.resize(cameraIndex + 1, 0);
    m_frameGens[cameraIndex] = m_frameGens[cameraIndex].toInt() + 1;

    ++m_frameGen;
    emit frameGensChanged();  // per-camera first so QML reads the updated list
    emit frameGenChanged();
}

} // namespace mosaic
