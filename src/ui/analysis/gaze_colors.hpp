#pragma once
#include <QColor>
#include <QString>

namespace mosaic {

// A 3D Gaze subject's colour, as the annotated videos draw it
// (analysis/gaze/render.py's PALETTE): by the number in its id, S1 first.
// Shared by the video's live overlay and the 3D Gaze view, so a person has
// one colour on the video and in the room.
inline QColor gaze_subject_color(const QString& id) {
    static const QColor kColors[] = {
        QColor(255, 200, 60), QColor(40, 160, 255), QColor(90, 230, 90),
        QColor(230, 90, 230), QColor(90, 255, 255), QColor(255, 120, 80),
    };
    bool ok     = false;
    const int n = id.mid(1).toInt(&ok);
    return kColors[ok && n > 0 ? (n - 1) % 6 : 0];
}

} // namespace mosaic
