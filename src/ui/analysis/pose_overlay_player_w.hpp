#pragma once
#include <QWidget>
#include <memory>

#include "analysis/expression_result.hpp"
#include "analysis/gaze2d_result.hpp"
#include "analysis/gaze_fusion_result.hpp"
#include "analysis/pose_analysis_result.hpp"
#include "analysis/rppg_result.hpp"
#include "analysis/skeleton3d_result.hpp"

namespace mosaic {

// Single-camera video playback with a pose-keypoint/skeleton overlay, a
// facial-expression bbox+label overlay, a gaze-fusion bbox+direction-arrow
// overlay, a 3D-pose-reconstruction reprojected-skeleton overlay, a
// remote-heart-rate (rPPG) face-ROI bbox + live BPM readout overlay, OR a
// calibration-free 2D gaze bbox+direction-arrow overlay, drawn in sync with
// the current playback position (all six are mutually exclusive — see
// set_pose_result()/set_expression_result()/set_gaze_result()/
// set_skeleton3d_result()/set_rppg_result()/set_gaze2d_result()).
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
    // set_pose_result() or set_expression_result() is also called.
    void set_video(const QString& videoPath);

    // Hides/shows just the video-display area, leaving the transport bar
    // (play/pause, scrubber, time) visible and fully functional — for
    // plugins whose "video" is actually a .wav file with nothing to render
    // (Diarization). Playback/seek/position_changed are unaffected.
    void set_video_surface_visible(bool visible);

    // Sets the pose data drawn as a skeleton overlay. Pass a
    // default-constructed (is_valid() == false) result to clear the
    // overlay. Mutually exclusive with set_expression_result()/
    // set_gaze_result()/set_skeleton3d_result()/set_rppg_result()/
    // set_gaze2d_result() — setting one clears every other, so the shared
    // overlay can never draw two stale overlays at once even if a caller
    // forgets to clear the others explicitly on a plugin switch.
    void set_pose_result(const PoseAnalysisResult& result);

    // Sets the expression data drawn as a bbox+label overlay. Pass a
    // default-constructed (is_valid() == false) result to clear the
    // overlay. Mutually exclusive with set_pose_result()/set_gaze_result()/
    // set_skeleton3d_result()/set_rppg_result()/set_gaze2d_result() (see
    // above).
    void set_expression_result(const ExpressionResult& result);

    // Sets the gaze-fusion data drawn as a bbox+direction-arrow overlay for
    // one specific camera (cameraIndex — the currently-loaded video's own
    // camera index, used to pick which GazeFusionFrame::perCamera entry
    // applies at each position). Pass a default-constructed
    // (is_valid() == false) result to clear the overlay. Mutually exclusive
    // with set_pose_result()/set_expression_result()/set_skeleton3d_result()/
    // set_rppg_result()/set_gaze2d_result() (see above).
    void set_gaze_result(const GazeFusionResult& result, int cameraIndex);

    // Sets the 3D-pose-reconstruction data drawn as a per-camera
    // REPROJECTED 2D skeleton overlay for one specific camera (cameraIndex
    // — picks which entry of each Skeleton3DPerson::reprojectedPx
    // applies). Consumes precomputed pixel coordinates only — no
    // calibration math happens here. Pass a default-constructed
    // (is_valid() == false) result to clear the overlay. Mutually exclusive
    // with set_pose_result()/set_expression_result()/set_gaze_result()/
    // set_rppg_result()/set_gaze2d_result() (see above).
    void set_skeleton3d_result(const Skeleton3DResult& result, int cameraIndex);

    // Sets the rPPG heart-rate data drawn as a face-ROI bbox + live BPM
    // readout overlay. cameraIndex exists only for API symmetry with the
    // other setters — RppgResult is already inherently single-camera (one
    // file per video), so it isn't used to filter anything here. Pass
    // a default-constructed (is_valid() == false) result to clear the
    // overlay. Mutually exclusive with set_pose_result()/
    // set_expression_result()/set_gaze_result()/set_skeleton3d_result()/
    // set_gaze2d_result() (see above).
    void set_rppg_result(const RppgResult& result, int cameraIndex);

    // Sets the calibration-free 2D gaze data drawn as a bbox+direction-
    // arrow overlay for one specific camera. cameraIndex exists only for
    // API symmetry with the other setters — Gaze2dResult is already
    // inherently single-camera (one file per video), so it isn't used to
    // filter anything here. Pass a default-constructed (is_valid() ==
    // false) result to clear the overlay. Mutually exclusive with
    // set_pose_result()/set_expression_result()/set_gaze_result()/
    // set_skeleton3d_result()/set_rppg_result() (see above).
    void set_gaze2d_result(const Gaze2dResult& result, int cameraIndex);

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
