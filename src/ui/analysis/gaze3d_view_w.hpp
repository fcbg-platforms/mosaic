#pragma once
#include <QWidget>
#include <cstdint>
#include <memory>
#include <optional>

#include "analysis/gaze_fusion_result.hpp"
#include "ui/analysis/room3d_stage.hpp"

namespace mosaic {

// The Multi-Camera Gaze Fusion results' room view, in 3D: every person's
// head with their gaze as a ray to where it lands, the named regions and the
// calibrated plane lit while looked at, each person's recent gaze points,
// on the same room as the 3D Pose view (the floor, the cameras with what
// each sees, the views, the view from any camera, Save image).
//
// Drawn with Qt Quick 3D (src/qml/Gaze3D.qml); what goes where is worked
// out in Gaze3DScene (gaze3d_scene.hpp).
//
// Usage:
//   auto* view = new Gaze3DViewW;
//   view->set_result(GazeFusionResult::load(jsonPath));
//   connect(player, &PoseOverlayPlayerW::position_changed, view,
//           &Gaze3DViewW::set_position_ms);
class Gaze3DViewW : public QWidget {
   public:
    explicit Gaze3DViewW(QWidget* parent = nullptr);
    ~Gaze3DViewW() override;

    // The result to show. `room`, the session's 3D Pose room when there is
    // one, sets up and the floor (see Gaze3DScene::set_result). An invalid
    // result clears the view.
    void set_result(const GazeFusionResult& result,
                    std::optional<Room3DStage::Basis> room = std::nullopt);

    // Which frame to draw, by player position; see
    // GazeFusionResult::frame_at_position_ms().
    void set_position_ms(int64_t positionMs);

    // Where Save image suggests saving (the session's folder).
    void set_image_folder(const QString& folder);

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
