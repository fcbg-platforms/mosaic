#pragma once
#include <QImage>
#include <QObject>
#include <QVariantList>
#include <memory>

namespace mosaic {

// Runs the Python pose-estimation subprocess and provides real-time keypoints.
//
// Start the worker once at app startup (or when pose overlay is first enabled).
// Submit BGR preview frames via submit_frame(); receive keypoints via pose_ready().
//
// The subprocess is expected at python/pose/frame_server.py (relative to exe dir,
// or set via MOSAIC_PYTHON env var for the interpreter path).
class PoseWorker : public QObject {
    Q_OBJECT
public:
    explicit PoseWorker(QObject* parent = nullptr);
    ~PoseWorker() override;

    // Start the Python subprocess. Returns false if the interpreter is not found.
    // interpeter: path to the Python executable inside the venv (or system Python).
    bool start(const QString& interpreter, const QString& scriptPath);
    void stop();
    [[nodiscard]] bool is_running() const;

public slots:
    void submit_frame(int cameraIndex, QImage frame);

signals:
    // keypoints is a QVariantList of QVariantMap: {x, y, z, visibility, name}
    void pose_ready(int cameraIndex, QVariantList keypoints);
    // gazeData is a QVariantMap: {face_box:{x,y,w,h}, left_iris:{x,y},
    //                              right_iris:{x,y}, gaze_dx, gaze_dy}
    void gaze_ready(int cameraIndex, QVariantMap gazeData);
    void process_error(QString message);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
