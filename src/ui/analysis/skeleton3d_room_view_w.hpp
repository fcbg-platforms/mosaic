#pragma once
#include <QWidget>
#include <cstdint>
#include <memory>

#include "analysis/skeleton3d_result.hpp"

namespace mosaic {

// Interactive, orbit-rotatable 3D room view for the 3D Pose Reconstruction
// plugin's results panel — renders a floor grid, each calibrated camera's
// static room position/label, and every currently-tracked person's 3D
// skeleton (color-coded by track_id), all at once (unlike GazeRoomViewW,
// which shows a single fused ray for one subject). Mouse-drag orbits
// yaw/pitch, wheel zooms distance, double-click resets the view —
// projection/depth-sorting math uses QVector3D/QMatrix4x4 (plain linear
// algebra utility types, no rendering context), with the actual drawing
// still plain QPainter — NOT Qt3D/OpenGL (same "no new Qt module for a 3D
// scene" decision already made and documented for item 19 — see
// GazeRoomViewW's own header doc).
//
// Usage:
//   auto* view = new Skeleton3DRoomViewW;
//   view->set_result(Skeleton3DResult::load(jsonPath));
//   connect(player, &PoseOverlayPlayerW::position_changed, view,
//           &Skeleton3DRoomViewW::set_position_ms);
class Skeleton3DRoomViewW : public QWidget {
   public:
    explicit Skeleton3DRoomViewW(QWidget* parent = nullptr);
    ~Skeleton3DRoomViewW() override;

    // Sets the full reconstruction result (cameras are static; the
    // auto-fit view target/default distance/default orientation are
    // recomputed once here from cameras' + every valid keypoint's extent —
    // NOT per paintEvent(), so the scale doesn't jump around during
    // playback, and the view resets to a sensible framing for the
    // newly-loaded result). Pass a default-constructed (is_valid() ==
    // false) result to clear the view.
    void set_result(const Skeleton3DResult& result);

    // Selects which fused frame to draw, by player position (ms since the
    // start of whichever video is currently loaded) — same
    // "player-position-0 ≈ result's first fused tick" approximation
    // PoseOverlayPlayerW's skeleton3d-mode overlay uses.
    void set_position_ms(int64_t positionMs);

    // Toggles between each keypoint's raw (Skeleton3DKeypoint::positionRoom,
    // the default) and centered-median-smoothed
    // (Skeleton3DKeypoint::positionRoomSmoothed) trajectory for drawing.
    // Room-view-only — the 2D video overlay (PoseOverlayPlayerW::
    // set_skeleton3d_result()) always draws raw positions regardless of
    // this setting, since its reprojected_px values are precomputed from
    // the raw point specifically to show ground truth against real video
    // pixels. Does not require a new set_result() call — takes effect on
    // the next repaint.
    void set_show_smoothed(bool showSmoothed);

    // Highlights two selected tracks (by trackId) with a connecting dashed
    // line + a live distance readout between their hip-midpoints — the
    // visual companion to the Analysis tab's Dyad Analysis panel
    // (analysis/dyadic_kinematics.hpp computes the same distance value from
    // the same raw positions, independently, for the chart/stats). Drawn
    // only for the currently-displayed frame, and only when both tracks
    // have a valid "left_hip"/"right_hip" pair there — resolved from the
    // already-loaded result's own keypoint_names() (cached by set_result()),
    // never a hardcoded COCO index. Pass -1 for either id to clear the
    // highlight (the default, no-dyad-selected state). Self-contained: does
    // not require a matching call to set_result() first or after — reads
    // straight from whatever result/frame is already current at paint time.
    void set_dyad_tracks(int trackIdA, int trackIdB);

   protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
