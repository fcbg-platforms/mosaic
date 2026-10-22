#pragma once
#include <QMap>
#include <QString>
#include <QVector>

namespace mosaic {

/// @brief What analysis/run_objects.py found in one camera's video
/// (``<session>/objects/video_N.objects.json``, schema mosaic-objects-v1):
/// the objects followed (SAM 3, found by name), when each was seen, and when
/// a person's hand touched it. Every frame's outlines are in a separate file,
/// not read here: the annotated video shows them. See
/// docs/math/object_tracking.rst.
class ObjectsResult {
   public:
    struct Object {
        int id{};
        QString label;
        double seenPct{};
        double travelPx{};
        int contacts{};
        QMap<QString, double> touchedSByPerson;   ///< person (Pose subject id, or ?) -> seconds
        QVector<QPair<double, double>> seenSpans; ///< when it was seen, session elapsed ms
    };
    struct Contact {
        int objectId{};
        QString person;
        double startMs{};
        double endMs{};
    };

    [[nodiscard]] static ObjectsResult load(const QString& path);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] const QVector<QString>& labels() const { return labels_; }
    [[nodiscard]] double video_start_ms() const { return videoStartMs_; }
    [[nodiscard]] bool pose_used() const { return poseUsed_; }
    /// Whether SAM 3 followed hands too, so touches were looked for without
    /// Pose (results from before that have touches only with Pose).
    [[nodiscard]] bool hands_followed() const { return handsFollowed_; }
    [[nodiscard]] QString annotated_video() const { return annotatedVideo_; }
    [[nodiscard]] const QVector<Object>& objects() const { return objects_; }
    [[nodiscard]] const QVector<Contact>& contacts() const { return contacts_; }

   private:
    bool valid_ = false;
    QVector<QString> labels_;
    double videoStartMs_{};
    bool poseUsed_      = false;
    bool handsFollowed_ = false;
    QString annotatedVideo_;
    QVector<Object> objects_;
    QVector<Contact> contacts_;
};

} // namespace mosaic
