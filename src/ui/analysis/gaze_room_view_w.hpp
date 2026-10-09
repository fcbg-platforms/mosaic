#pragma once
#include <QWidget>
#include <cstdint>
#include <memory>

#include "analysis/gaze_fusion_result.hpp"

namespace mosaic {

// The room seen from above, for the Multi-Camera Gaze Fusion results panel:
// cameras, named regions, every subject at the current position, their gaze
// rays and gaze points, and below it what each subject looks at. "Up" is the
// calibrated plane's normal when there is one, else the reference camera's
// up, the same choice as gaze_fusion/room_topdown.mp4 (analysis/gaze/
// render.py), so the two show the room the same way round. Plain QPainter,
// no Qt3D/OpenGL.
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

    // Selects which frame to draw, by player position (ms since the start of
    // the loaded synced or annotated video); see
    // GazeFusionResult::frame_at_position_ms().
    void set_position_ms(int64_t positionMs);

   protected:
    void paintEvent(QPaintEvent*) override;

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
