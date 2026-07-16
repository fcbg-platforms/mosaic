#pragma once
#include "analysis/pose_analysis_result.hpp"
#include <QWidget>
#include <memory>

namespace mosaic {

// Single-camera video playback with a pose-keypoint/skeleton overlay drawn
// in sync with the current playback position.
//
// Not a reuse of SessionPlayerW's CameraSlotW: that class is coupled to
// multi-camera master-clock sync, which doesn't apply to this single-video,
// overlay-driven view. Follows the same underlying QMediaPlayer + QVideoSink
// pattern.
//
// Usage:
//   auto* player = new PoseOverlayPlayerW;
//   player->set_video(videoPath);
//   player->set_pose_result(PoseAnalysisResult::load(poseJsonPath));
//   connect(player, &PoseOverlayPlayerW::position_changed, chart,
//           &MetricsChartW::set_playhead_ms);
class PoseOverlayPlayerW : public QWidget {
    Q_OBJECT
public:
    explicit PoseOverlayPlayerW(QWidget* parent = nullptr);
    ~PoseOverlayPlayerW() override;

    // Loads a video file for playback. The overlay is blank until
    // set_pose_result() is also called.
    void set_video(const QString& videoPath);

    // Sets the pose data drawn as an overlay. Pass a default-constructed
    // (is_valid() == false) result to clear the overlay.
    void set_pose_result(const PoseAnalysisResult& result);

    [[nodiscard]] int64_t position_ms() const;
    [[nodiscard]] int64_t duration_ms() const;

public slots:
    void play();
    void pause();
    void seek(int64_t positionMs);

signals:
    // Emitted as playback advances (and on manual seeks), so a companion
    // metrics chart can keep its playhead in sync.
    void position_changed(int64_t positionMs);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
