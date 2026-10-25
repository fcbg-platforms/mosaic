#pragma once
#include <QColor>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QVector3D>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "analysis/skeleton3d_result.hpp"
#include "ui/analysis/room3d_stage.hpp"

namespace mosaic {

// The 3D Pose room view's scene, for its Qt Quick 3D page (qml/Room3D.qml).
//
// Turns the result into what the page draws: for the frame on show, each
// bone (centre, rotation, length), joint and head of every person shown, the
// trails, each person's whole path, the two-person line, and the labels'
// anchor points. The room itself (the floor, the cameras, the framing) is
// its stage (room3d_stage.hpp), shared with the 3D Gaze view. The page only
// lays these out; the maths stays here, testable.
class Room3DScene : public Room3DMeshSource {
    Q_OBJECT
    Q_PROPERTY(QObject* stage READ stage_object CONSTANT)
    Q_PROPERTY(QVariantList people READ people NOTIFY peopleChanged)
    Q_PROPERTY(bool hasResult READ has_result NOTIFY resultChanged)
    Q_PROPERTY(QVariantList bones READ bones NOTIFY frameChanged)
    Q_PROPERTY(QVariantList joints READ joints NOTIFY frameChanged)
    Q_PROPERTY(QVariantList heads READ heads NOTIFY frameChanged)
    Q_PROPERTY(QVariantList tags READ tags NOTIFY frameChanged)
    Q_PROPERTY(QVariantMap dyad READ dyad NOTIFY frameChanged)
    Q_PROPERTY(QString info READ info NOTIFY frameChanged)
    Q_PROPERTY(QVector3D followTarget READ follow_target NOTIFY frameChanged)
    // Whether the followed person is in the frame on show (followTarget is
    // then where they are).
    Q_PROPERTY(bool followFound READ follow_found NOTIFY frameChanged)
    Q_PROPERTY(int follow READ follow WRITE set_follow NOTIFY peopleChanged)
    Q_PROPERTY(bool trails READ trails WRITE set_trails NOTIFY frameChanged)
    Q_PROPERTY(bool paths READ paths WRITE set_paths NOTIFY pathsChanged)

   public:
    explicit Room3DScene(QObject* parent = nullptr);

    void set_result(const Skeleton3DResult& result);
    void set_position_ms(int64_t positionMs);
    void set_show_smoothed(bool smoothed);
    void set_dyad(int personA, int personB);

    // Shown on the page's People panel; hidden people are left out of
    // everything drawn.
    Q_INVOKABLE void set_shown(int person, bool shown);
    void set_follow(int person);
    void set_trails(bool on);
    void set_paths(bool on);

    [[nodiscard]] Room3DStage& stage() { return stage_; }
    [[nodiscard]] const Room3DStage& stage() const { return stage_; }
    [[nodiscard]] QObject* stage_object() { return &stage_; }
    [[nodiscard]] QVariantList people() const;
    [[nodiscard]] bool has_result() const { return result_.is_valid(); }
    [[nodiscard]] QVariantList bones() const { return bones_; }
    [[nodiscard]] QVariantList joints() const { return joints_; }
    [[nodiscard]] QVariantList heads() const { return heads_; }
    [[nodiscard]] QVariantList tags() const { return tags_; }
    [[nodiscard]] QVariantMap dyad() const { return dyad_; }
    [[nodiscard]] QString info() const { return info_; }
    [[nodiscard]] QVector3D follow_target() const { return followTarget_; }
    [[nodiscard]] bool follow_found() const { return followFound_; }
    [[nodiscard]] int follow() const { return follow_; }
    [[nodiscard]] bool trails() const { return trails_; }
    [[nodiscard]] bool paths() const { return paths_; }
    // The tick on show, -1 for none (Save image names the file after it).
    [[nodiscard]] int64_t tick() const { return tick_; }

    // Room frame (mm) to the scene, as the stage turns it.
    [[nodiscard]] QVector3D to_scene(const Skeleton3DVec3& room) const {
        return stage_.to_scene(room);
    }
    [[nodiscard]] QVector3D dir_to_scene(const QVector3D& roomDir) const {
        return stage_.dir_to_scene(roomDir);
    }

    // Its meshes (mesh()): the trails, the paths and the torsos.

   signals:
    void resultChanged();
    void frameChanged();
    void peopleChanged();
    void pathsChanged();

   private:
    void rebuild_frame();
    void rebuild_static();
    void rebuild_paths();
    // A keypoint and the hips' midpoint in the scene, as drawn (smoothed
    // when Show smoothed is on); none when not valid.
    [[nodiscard]] std::optional<QVector3D> point(const Skeleton3DPerson& p, int i) const;
    [[nodiscard]] std::optional<QVector3D> hips(const Skeleton3DPerson& p) const;

    Skeleton3DResult result_;
    int64_t positionMs_ = 0;
    bool smoothed_      = false;
    bool trails_        = true;
    bool paths_         = false;
    int dyadA_ = -1, dyadB_ = -1;
    QSet<int> hidden_;
    int follow_ = -1;

    Room3DStage stage_;
    QMap<int, double> seconds_; // person -> seconds in 3D
    QVector<int> headIdx_;
    int lHip_ = -1, rHip_ = -1, lSh_ = -1, rSh_ = -1, nose_ = -1, lEar_ = -1, rEar_ = -1;

    QVariantList bones_, joints_, heads_, tags_;
    QVariantMap dyad_;
    QString info_;
    QVector3D followTarget_;
    bool followFound_ = false;
    int64_t tick_     = -1;
};

} // namespace mosaic
