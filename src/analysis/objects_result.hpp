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
        /// The name given in the app ("red ball"); empty until then.
        QString name;
        double seenPct{};
        double travelPx{};
        int contacts{};
        QMap<QString, double> touchedSByPerson;   ///< person (Pose subject id, or ?) -> seconds
        QVector<QPair<double, double>> seenSpans; ///< when it was seen, session elapsed ms

        /// "toy 2", or "red ball (toy 2)" once named: the label and id stay,
        /// as the annotated video shows them.
        [[nodiscard]] QString display_name() const {
            const QString found = QString("%1 %2").arg(label).arg(id);
            return name.isEmpty() ? found : QString("%1 (%2)").arg(name, found);
        }
    };
    struct Contact {
        int objectId{};
        QString person;
        double startMs{};
        double endMs{};
    };

    [[nodiscard]] static ObjectsResult load(const QString& path);

    /// Writes the names given in the app into the result at `path`, by
    /// object id: a "name" on each of its summary objects, removed for an
    /// empty name. The rest of the file is kept as it is. Returns false, and
    /// leaves the file alone, when it cannot be read or written.
    [[nodiscard]] static bool save_names(const QString& path, const QMap<int, QString>& names);

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
