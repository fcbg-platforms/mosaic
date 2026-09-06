#pragma once
#include <QColor>
#include <QString>

#include "analysis/pose_analysis_result.hpp"

namespace mosaic {

// Per-detected-person color palette, shared by every "distinguish multiple
// simultaneously-visible people" UI in the Analysis tab: the pose3d 3D room
// view and video overlay (by track_id), and the Pose plugin's multi-subject
// chart, chips and 2D skeleton overlay (by SubjectChoice). Previously
// duplicated verbatim in skeleton3d_room_view_w.cpp and
// pose_overlay_player_w.cpp — consolidated here once a 3rd usage site needed
// it.
inline const QColor kSubjectColors[] = {
    QColor("#00dcff"), QColor("#ffdd44"), QColor("#ff4488"),
    QColor("#44cc44"), QColor("#cc44cc"), QColor("#ff8844"),
};

[[nodiscard]] inline QColor subject_color(int index) {
    return kSubjectColors[((index % 6) + 6) % 6];
}

// One selectable person in a Pose result: their identity, plus their position
// in PoseAnalysisResult::subject_ids().
//
// The ordinal exists because the raw id is a poor thing to show or to colour
// by. A tracker's ids climb over a session and skip freely (1, 2, 5, 9), so
// labelling with them would produce a "Subject 9" the user can't relate to a
// chip row of three, and colouring by them collides id 1 with id 7 — and, via
// subject_color()'s deliberate negative-safe modulo, id -1 with id 5. The
// ordinal is dense, 1-based, and fixed for the lifetime of a loaded result,
// so it is stable per person in exactly the way per-frame detection order was
// not. For a pre-tracking file (ids == dense array positions) ordinal-1 equals
// the old index, so legacy results keep their exact previous labels and
// colours.
struct SubjectChoice {
    SubjectId id;
    int ordinal = 1; // 1-based position in PoseAnalysisResult::subject_ids()
};

[[nodiscard]] inline QColor subject_color(SubjectChoice choice) {
    return subject_color(choice.ordinal - 1);
}

// "Subject 3". Only ever built for a subject that survived
// PoseAnalysisResult::subject_ids()'s filter, i.e. one that can genuinely be
// followed across frames — untracked detections are excluded there and are
// rendered anonymously by the overlay instead.
[[nodiscard]] inline QString subject_label(SubjectChoice choice) {
    return QString("Subject %1").arg(choice.ordinal);
}

// The selectable people in a result, in subject_ids() order.
[[nodiscard]] inline QVector<SubjectChoice> subject_choices(const PoseAnalysisResult& result) {
    QVector<SubjectChoice> out;
    const auto& ids = result.subject_ids();
    out.reserve(ids.size());
    for (int i = 0; i < ids.size(); ++i) {
        out.push_back({ids[i], i + 1});
    }
    return out;
}

} // namespace mosaic
