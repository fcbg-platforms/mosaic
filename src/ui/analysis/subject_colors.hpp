#pragma once
#include <QColor>

namespace mosaic {

// Per-detected-person color palette, shared by every "distinguish multiple
// simultaneously-visible people" UI in the Analysis tab: the pose3d 3D room
// view and video overlay (by track_id), and the Pose plugin's multi-subject
// chart + 2D skeleton overlay (by subject index). Previously duplicated
// verbatim in skeleton3d_room_view_w.cpp and pose_overlay_player_w.cpp —
// consolidated here once a 3rd usage site needed it.
inline const QColor kSubjectColors[] = {
    QColor("#00dcff"), QColor("#ffdd44"), QColor("#ff4488"),
    QColor("#44cc44"), QColor("#cc44cc"), QColor("#ff8844"),
};

[[nodiscard]] inline QColor subject_color(int index) {
    return kSubjectColors[((index % 6) + 6) % 6];
}

} // namespace mosaic
