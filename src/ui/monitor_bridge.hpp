#pragma once
#include "core/settings.hpp"
#include "record/record_manager.hpp"
#include "video/video_feed_provider.hpp"
#include <QImage>
#include <QObject>
#include <QString>
#include <QVariantList>

namespace mosaic {

// QObject registered as QML context property "backend".
// Exposes RecordManager state, live camera frames, and pose keypoints to QML.

class MonitorBridge : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool    recording    READ isRecording  NOTIFY recordingChanged)
    Q_PROPERTY(int     elapsedMs    READ elapsedMs    NOTIFY elapsedMsChanged)
    Q_PROPERTY(int     cameraCount  READ cameraCount  NOTIFY cameraCountChanged)
    Q_PROPERTY(QString sessionPath  READ sessionPath  NOTIFY sessionPathChanged)
    // Global counter — increments whenever any camera delivers a frame (kept for compat).
    Q_PROPERTY(int     frameGen     READ frameGen     NOTIFY frameGenChanged)
    // Per-camera counters: frameGens[i] increments only when camera i gets a new frame,
    // so each QML slot only reloads when its own camera produces new data.
    Q_PROPERTY(QVariantList frameGens READ frameGens NOTIFY frameGensChanged)
    // Per-camera pose keypoints: allPoseKeypoints[camIdx] is a list of {x,y,z,visibility,name}.
    Q_PROPERTY(QVariantList allPoseKeypoints READ allPoseKeypoints NOTIFY poseKeypointsChanged)
    // Per-camera gaze data: allGazeData[camIdx] is a map {face_box,left_iris,right_iris,gaze_dx,gaze_dy}.
    Q_PROPERTY(QVariantList allGazeData READ allGazeData NOTIFY gazeDataChanged)

public:
    explicit MonitorBridge(RecordManager*       recordMgr,
                            const VideoSettings& videoSettings,
                            QObject*             parent = nullptr);

    // Q_PROPERTY readers
    [[nodiscard]] bool          isRecording()     const;
    [[nodiscard]] int           elapsedMs()       const;
    [[nodiscard]] int           cameraCount()     const;
    [[nodiscard]] QString       sessionPath()     const;
    [[nodiscard]] int           frameGen()        const;
    [[nodiscard]] QVariantList  frameGens()       const;
    [[nodiscard]] QVariantList  allPoseKeypoints() const;
    [[nodiscard]] QVariantList  allGazeData()      const;

    // Called by VideoSettingsW when cameras are added/removed
    void set_camera_count(int count);

    // Called from MainWindow after the QML engine is set up
    void set_feed_provider(VideoFeedProvider* provider);

public slots:
    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();

    // Connected to VideoManager::frame_preview (already on main thread via queued)
    void on_frame_preview(int cameraIndex, QImage frame);

    // Connected to PoseWorker::pose_ready — keypoints is a QVariantList of QVariantMap
    void on_pose_ready(int cameraIndex, QVariantList keypoints);
    // Connected to PoseWorker::gaze_ready
    void on_gaze_ready(int cameraIndex, QVariantMap gazeData);

signals:
    void recordingChanged();
    void elapsedMsChanged();
    void cameraCountChanged();
    void sessionPathChanged();
    void frameGenChanged();
    void frameGensChanged();
    void poseKeypointsChanged();
    void gazeDataChanged();

private:
    RecordManager*       m_rm;
    const VideoSettings& m_videoSettings;
    int                  m_cameraCount{0};
    QString              m_sessionPath;
    VideoFeedProvider*   m_feedProvider{nullptr};
    int                  m_frameGen{0};
    QVariantList         m_frameGens;          // per-camera generation counters
    QVariantList         m_allPoseKeypoints;   // index = cameraIndex
    QVariantList         m_allGazeData;        // index = cameraIndex
};

} // namespace mosaic
