#pragma once
#include "analysis/gaze_fusion_result.hpp"
#include <QWidget>
#include <cstdint>
#include <memory>

namespace mosaic {

// Fixed top-down (room X/Y) orthographic room-view widget for the
// Multi-Camera Gaze Fusion plugin's results panel — renders the defined
// reference plane, each camera's static room position, the current fused
// frame's ray (origin → target, or a fixed extension if no target), and
// each contributing camera's individual ray (thin/low-opacity, for visual
// triangulation-spread QA). Plain QPainter, no Qt3D/OpenGL — scope is one
// flat plane plus a handful of lines/points, not a full 3D scene (see the
// item 19 plan's explicit "no QOpenGLWidget" decision).
//
// Usage:
//   auto* view = new GazeRoomViewW;
//   view->set_result(GazeFusionResult::load(jsonPath));
//   connect(player, &PoseOverlayPlayerW::position_changed, view,
//           &GazeRoomViewW::set_position_ms);
class GazeRoomViewW : public QWidget {
public:
    explicit GazeRoomViewW(QWidget* parent = nullptr);
    ~GazeRoomViewW() override;

    // Sets the full fusion result (cameras + plane are static; the
    // auto-fit view bounds are recomputed once here from their extent plus
    // every frame's ray/target extent — NOT per paintEvent(), so the scale
    // doesn't jump around during playback). Pass a default-constructed
    // (is_valid() == false) result to clear the view.
    void set_result(const GazeFusionResult& result);

    // Selects which fused frame to draw, by player position (ms since the
    // start of whichever video is currently loaded) — same
    // "player-position-0 ≈ result's first fused tick" approximation
    // PoseOverlayPlayerW's gaze-mode overlay uses (see its Impl::
    // gaze_timestamp_estimate() doc comment).
    void set_position_ms(int64_t positionMs);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
