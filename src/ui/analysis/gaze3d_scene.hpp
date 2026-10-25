#pragma once
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QVector3D>
#include <cstdint>
#include <optional>

#include "analysis/gaze_fusion_result.hpp"
#include "ui/analysis/room3d_stage.hpp"

namespace mosaic {

// The 3D Gaze view's scene, for its Qt Quick 3D page (qml/Gaze3D.qml) on
// the room's stage (room3d_stage.hpp, shared with the 3D Pose room view).
//
// For the tick on show: each person's head, their gaze as a ray from the
// eyes to where it lands, the gaze point; the named regions and the
// calibrated plane, lit while someone looks at them; a label per person
// saying what they look at; and each person's recent gaze points (trails).
// The page only lays these out; the maths stays here, testable.
class Gaze3DScene : public Room3DMeshSource {
    Q_OBJECT
    Q_PROPERTY(QObject* stage READ stage_object CONSTANT)
    Q_PROPERTY(bool hasResult READ has_result NOTIFY resultChanged)
    Q_PROPERTY(QVariantList people READ people NOTIFY peopleChanged)
    Q_PROPERTY(QVariantList heads READ heads NOTIFY frameChanged)
    Q_PROPERTY(QVariantList rays READ rays NOTIFY frameChanged)
    Q_PROPERTY(QVariantList points READ points NOTIFY frameChanged)
    Q_PROPERTY(QVariantList regions READ regions NOTIFY frameChanged)
    Q_PROPERTY(QVariantMap plane READ plane NOTIFY frameChanged)
    Q_PROPERTY(QVariantList tags READ tags NOTIFY frameChanged)
    Q_PROPERTY(QString info READ info NOTIFY frameChanged)
    Q_PROPERTY(QVector3D followTarget READ follow_target NOTIFY frameChanged)
    Q_PROPERTY(bool followFound READ follow_found NOTIFY frameChanged)
    Q_PROPERTY(QString follow READ follow WRITE set_follow NOTIFY peopleChanged)
    Q_PROPERTY(bool trails READ trails WRITE set_trails NOTIFY frameChanged)

   public:
    explicit Gaze3DScene(QObject* parent = nullptr);

    // A new result. `room`, the session's 3D Pose room (Room3DStage::
    // basis_of() of its result) when there is one, sets which way is up and
    // where the floor is, so both views share the room; without it, up is
    // the calibrated plane's normal (turned towards the cameras) or the
    // cameras', and the floor 2 m below them.
    void set_result(const GazeFusionResult& result,
                    std::optional<Room3DStage::Basis> room = std::nullopt);
    void set_position_ms(int64_t positionMs);

    // The People panel: hidden people are left out of everything drawn.
    Q_INVOKABLE void set_shown(const QString& id, bool shown);
    void set_follow(const QString& id);
    void set_trails(bool on);

    [[nodiscard]] Room3DStage& stage() { return stage_; }
    [[nodiscard]] const Room3DStage& stage() const { return stage_; }
    [[nodiscard]] QObject* stage_object() { return &stage_; }
    [[nodiscard]] bool has_result() const { return result_.is_valid(); }
    [[nodiscard]] QVariantList people() const;
    [[nodiscard]] QVariantList heads() const { return heads_; }
    [[nodiscard]] QVariantList rays() const { return rays_; }
    [[nodiscard]] QVariantList points() const { return points_; }
    [[nodiscard]] QVariantList regions() const { return regions_; }
    [[nodiscard]] QVariantMap plane() const { return plane_; }
    [[nodiscard]] QVariantList tags() const { return tags_; }
    [[nodiscard]] QString info() const { return info_; }
    [[nodiscard]] QVector3D follow_target() const { return followTarget_; }
    [[nodiscard]] bool follow_found() const { return followFound_; }
    [[nodiscard]] QString follow() const { return follow_; }
    [[nodiscard]] bool trails() const { return trails_; }
    // The tick on show, -1 for none (Save image names the file after it).
    [[nodiscard]] int64_t tick() const { return tick_; }

    // Its mesh (mesh()): the trails.

   signals:
    void resultChanged();
    void frameChanged();
    void peopleChanged();

   private:
    void rebuild_frame();
    [[nodiscard]] QColor color_of(const QString& id) const;

    Room3DStage stage_;
    GazeFusionResult result_;
    int64_t positionMs_ = 0;
    QSet<QString> hidden_;
    QString follow_;
    bool trails_ = true;
    QVector<QPair<QString, double>> seen_; // id -> seconds seen, in the result's order

    QVariantList heads_, rays_, points_, regions_, tags_;
    QVariantMap plane_;
    QString info_;
    QVector3D followTarget_;
    bool followFound_ = false;
    int64_t tick_     = -1;
};

} // namespace mosaic
