#pragma once
#include <QWidget>
#include <cstdint>
#include <memory>

#include "analysis/skeleton3d_result.hpp"

namespace mosaic {

// The 3D Pose Reconstruction plugin's room view: every person upright on a
// lit floor with a metre grid, their shadows, the cameras as small 3D
// cameras with what each one sees, which way each face points, trails, and
// a joint's room coordinates on hover; 3D, Top, Front and Side views, and a
// People panel to hide or follow a person.
//
// Drawn with Qt Quick 3D (src/qml/Room3D.qml): real shadows, ambient
// occlusion and smooth edges come from it. The scene's contents (where each
// bone, joint, head and camera goes) are worked out in Room3DScene
// (room3d_scene.hpp), so the page only lays them out.
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

    // Sets the full reconstruction result. Up, the floor and the framing are
    // worked out once here, not per frame, so the view does not jump during
    // playback. Pass a default-constructed (is_valid() == false) result to
    // clear the view.
    void set_result(const Skeleton3DResult& result);

    // Selects which fused frame to draw, by player position (ms since the
    // start of whichever video is loaded): the same "player position 0 is
    // about the result's first tick" approximation the 2D overlay uses.
    void set_position_ms(int64_t positionMs);

    // Raw (the default) or smoothed keypoints. Room view only: the 2D video
    // overlay always draws raw positions, whose reprojections are the ground
    // truth against the video's pixels.
    void set_show_smoothed(bool showSmoothed);

    // Joins two people (their numbers) at the hips with their distance,
    // when both are in the frame on show: the companion to the Dyad Analysis
    // charts. -1 for either clears it. The distance printed is from raw
    // positions, as the Dyad Analysis numbers are.
    void set_dyad_tracks(int personA, int personB);

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
