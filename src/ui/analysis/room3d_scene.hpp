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
#include <vector>

#include "analysis/skeleton3d_result.hpp"

namespace mosaic {

// The 3D Pose room view's scene, for its Qt Quick 3D page (qml/Room3D.qml).
//
// Turns the result into what the page draws: for the frame on show, each
// bone (centre, rotation, length), joint and head of every person shown, the
// cameras, the trails, the two-person line, and the labels' anchor points.
// The page only lays these out; the maths stays here, testable.
//
// The scene is Y-up, in millimetres, with the floor at y = 0: the room
// frame (the reference camera's own, x right, y down, z forward) is turned
// so the room's up (found from the people and cameras, room_view_math.hpp)
// is +y, its forward along the floor is -z, and its right +x. Coordinates
// shown to the user stay the room frame's, in metres, as in every export.
class Room3DScene : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList cameras READ cameras NOTIFY resultChanged)
    Q_PROPERTY(QVariantList people READ people NOTIFY peopleChanged)
    Q_PROPERTY(float floorSize READ floor_size NOTIFY resultChanged)
    Q_PROPERTY(QVector3D home READ home NOTIFY resultChanged)
    // The floor plate's and the grid's centre: `home` on whole metres.
    Q_PROPERTY(QVector3D floorCentre READ floor_centre NOTIFY resultChanged)
    Q_PROPERTY(float extent READ extent NOTIFY resultChanged)
    Q_PROPERTY(bool hasResult READ has_result NOTIFY resultChanged)
    Q_PROPERTY(QString frameCamera READ frame_camera NOTIFY resultChanged)
    // The room frame's x, y and z in the scene, for the axes marker.
    Q_PROPERTY(QVariantList axes READ axes NOTIFY resultChanged)
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

    [[nodiscard]] QVariantList cameras() const { return cameras_; }
    [[nodiscard]] QVariantList people() const;
    [[nodiscard]] float floor_size() const { return floorSize_; }
    [[nodiscard]] QVector3D home() const { return home_; }
    [[nodiscard]] QVector3D floor_centre() const {
        return {std::round(home_.x() / 1000.0f) * 1000.0f, 0.0f,
                std::round(home_.z() / 1000.0f) * 1000.0f};
    }
    [[nodiscard]] float extent() const { return extent_; }
    [[nodiscard]] bool has_result() const { return result_.is_valid(); }
    [[nodiscard]] QString frame_camera() const { return frameCamera_; }
    [[nodiscard]] QVariantList axes() const;
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

    // Room frame (mm) to scene (mm, Y-up, floor at 0), and the room frame
    // direction to the scene's.
    [[nodiscard]] QVector3D to_scene(const Skeleton3DVec3& room) const;
    [[nodiscard]] QVector3D dir_to_scene(const QVector3D& roomDir) const;

    // Line and triangle meshes, as x y z r g b a per vertex: the floor grid,
    // the camera pyramids (lines and their translucent far faces), the
    // trails, and the torsos.
    struct Mesh {
        std::vector<float> vertices;
        bool triangles = false;
    };
    [[nodiscard]] const Mesh& mesh(const QString& part) const;

   signals:
    void resultChanged();
    void frameChanged();
    void peopleChanged();

   private:
    void rebuild_frame();
    void rebuild_static();

    Skeleton3DResult result_;
    int64_t positionMs_ = 0;
    bool smoothed_      = false;
    bool trails_        = true;
    int dyadA_ = -1, dyadB_ = -1;
    QSet<int> hidden_;
    int follow_ = -1;

    // Room basis: up, forward along the floor, right; and the floor's height.
    QVector3D up_{0, -1, 0}, fwd_{0, 0, 1}, right_{1, 0, 0};
    float floorH_ = 0.0f;
    QVector3D home_;
    float extent_    = 2000.0f;
    float floorSize_ = 6000.0f;
    QString frameCamera_;
    QMap<int, double> seconds_; // person -> seconds in 3D
    QVector<int> headIdx_;
    int lHip_ = -1, rHip_ = -1, lSh_ = -1, rSh_ = -1, nose_ = -1, lEar_ = -1, rEar_ = -1;

    QVariantList cameras_, bones_, joints_, heads_, tags_;
    QVariantMap dyad_;
    QString info_;
    QVector3D followTarget_;
    bool followFound_ = false;
    QMap<QString, Mesh> meshes_;
};

} // namespace mosaic
