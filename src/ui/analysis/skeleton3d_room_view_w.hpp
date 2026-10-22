#pragma once
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPainter>
#include <cstdint>
#include <memory>

#include "analysis/skeleton3d_result.hpp"

namespace mosaic {

// Interactive 3D room view for the 3D Pose Reconstruction plugin's results:
// every tracked person's skeleton as shaded limbs and joints (colour by
// track_id), a metric floor grid, the calibrated cameras and what each one
// sees, fading movement trails, and a joint's room coordinates on hover.
//
// Drawn with OpenGL (QOpenGLWidget, from Qt's OpenGLWidgets module, which
// comes with Qt's base install), so depth, lighting and smooth edges come from the GPU. Text
// (labels, the hover readout, the axes) is drawn by a transparent child
// widget over it, with QPainter. On a
// PC without a capable driver (Remote Desktop), Qt's software OpenGL
// (opengl32sw.dll, deployed with MOSAIC) stands in.
//
// The room frame is the reference camera's own (x right, y down, z
// forward), so "up" and the floor are found from the people and cameras
// (room_view_math.hpp): people are drawn upright on a floor whichever way
// that camera was mounted. Coordinates shown are always the room frame's,
// in metres, as in every export.
//
// Views (buttons at the top right): 3D (orbit: drag to turn, right-drag to
// pan, scroll to zoom), and Top, Front and Side, flat (orthographic, drag to
// pan). Double-click returns to the view's starting framing.
//
// Usage:
//   auto* view = new Skeleton3DRoomViewW;
//   view->set_result(Skeleton3DResult::load(jsonPath));
//   connect(player, &PoseOverlayPlayerW::position_changed, view,
//           &Skeleton3DRoomViewW::set_position_ms);
class Skeleton3DRoomViewW : public QOpenGLWidget, protected QOpenGLFunctions {
   public:
    explicit Skeleton3DRoomViewW(QWidget* parent = nullptr);
    ~Skeleton3DRoomViewW() override;

    // Sets the full reconstruction result. Up, the floor and the framing are
    // worked out once here, not per frame, so the view does not jump during
    // playback. Pass a default-constructed (is_valid() == false) result to
    // clear the view.
    void set_result(const Skeleton3DResult& result);

    // Selects which fused frame to draw, by player position (ms since the
    // start of whichever video is loaded): the same "player position 0 is
    // about the result's first tick" approximation the 2D overlay uses.
    void set_position_ms(int64_t positionMs);

    // Raw (Skeleton3DKeypoint::positionRoom, the default) or smoothed
    // (positionRoomSmoothed) keypoints. Room view only: the 2D video overlay
    // always draws raw positions, whose reprojections are the ground truth
    // against the video's pixels.
    void set_show_smoothed(bool showSmoothed);

    // Highlights two tracks (by trackId) with a line between their hip
    // midpoints and the distance, when both have both hips in the frame on
    // show: the companion to the Dyad Analysis panel. -1 for either clears
    // it. The distance printed is always from raw positions, as the Dyad
    // Analysis numbers are.
    void set_dyad_tracks(int trackIdA, int trackIdB);

   protected:
    void initializeGL() override;
    void resizeEvent(QResizeEvent* e) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

   private:
    struct Impl;
    std::unique_ptr<Impl> d;

    void apply_preset(int preset);
    void refresh();
    void paint_overlay(QPainter& p);
};

} // namespace mosaic
