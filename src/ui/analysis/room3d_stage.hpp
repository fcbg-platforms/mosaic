#pragma once
#include <QColor>
#include <QMap>
#include <QObject>
#include <QVariantList>
#include <QVector3D>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include "analysis/skeleton3d_result.hpp"

namespace mosaic {

// What a 3D room page draws its lines and surfaces from: meshes by part
// name. The room (Room3DStage) and each page's contents (Room3DScene's
// people) are both sources; SceneMesh (room3d_mesh.hpp) follows one part of
// one source.
class Room3DMeshSource : public QObject {
    Q_OBJECT

   public:
    using QObject::QObject;

    // x y z r g b a per vertex; lines, or triangles.
    struct Mesh {
        std::vector<float> vertices;
        bool triangles = false;
    };
    [[nodiscard]] const Mesh& mesh(const QString& part) const {
        static const Mesh kEmpty;
        const auto it = meshes_.constFind(part);
        return it == meshes_.cend() ? kEmpty : it.value();
    }

   signals:
    // A part was rebuilt; an empty part is all of them.
    void meshChanged(const QString& part);

   protected:
    void set_mesh(const QString& part, Mesh mesh) {
        meshes_[part] = std::move(mesh);
        emit meshChanged(part);
    }

   private:
    QMap<QString, Mesh> meshes_;
};

// The room of a 3D page (qml/RoomStage.qml): which way is up, the floor
// and its grid, the framing, and the cameras with what each one sees. The
// 3D Pose room view and the 3D Gaze view each put their own contents on it.
//
// The scene is Y-up, in millimetres, with the floor at y = 0: the room
// frame (the reference camera's own, x right, y down, z forward) is turned
// so the room's up is +y, its forward along the floor -z and its right +x.
// Coordinates shown to the user stay the room frame's, in metres, as in
// every export.
class Room3DStage : public Room3DMeshSource {
    Q_OBJECT
    Q_PROPERTY(QVariantList cameras READ cameras NOTIFY roomChanged)
    Q_PROPERTY(float floorSize READ floor_size NOTIFY roomChanged)
    Q_PROPERTY(QVector3D home READ home NOTIFY roomChanged)
    // The floor plate's and the grid's centre: `home` on whole metres.
    Q_PROPERTY(QVector3D floorCentre READ floor_centre NOTIFY roomChanged)
    Q_PROPERTY(float extent READ extent NOTIFY roomChanged)
    Q_PROPERTY(bool hasRoom READ has_room NOTIFY roomChanged)
    Q_PROPERTY(QString frameCamera READ frame_camera NOTIFY roomChanged)
    // The room frame's x, y and z in the scene, for the axes marker.
    Q_PROPERTY(QVariantList axes READ axes NOTIFY roomChanged)

   public:
    using Room3DMeshSource::Room3DMeshSource;

    // The room's up and forward along the floor (room frame, unit vectors),
    // and the floor's height along up (mm, as dot(point, up)).
    struct Basis {
        QVector3D up{0, -1, 0};
        QVector3D fwd{0, 0, 1};
        float floorH = 0.0f;
    };
    // From a 3D Pose result: up from the people (else the cameras), the
    // floor from the ankles (room_view_math.hpp).
    [[nodiscard]] static Basis basis_of(const Skeleton3DResult& result);
    // From the cameras alone, for a result without people's bodies (3D
    // Gaze): up as given (room frame, unit), else from the cameras'
    // image-up; the floor 2 m below the lowest camera.
    [[nodiscard]] static Basis basis_of_cameras(const QVector<Skeleton3DRoomCamera>& cameras,
                                                std::optional<QVector3D> up = std::nullopt);

    // A new room: its basis, its cameras, and the room-frame points (mm)
    // the framing is to hold (where the people were); the cameras' when
    // there are none.
    void set_room(const Basis& basis, const QVector<Skeleton3DRoomCamera>& cameras,
                  const std::vector<Skeleton3DVec3>& points);
    // No room: nothing drawn.
    void clear_room();

    // The page's Save image: the widget around the page saves it.
    Q_INVOKABLE void request_save_image() { emit saveImageRequested(); }

    [[nodiscard]] QVariantList cameras() const { return cameras_; }
    [[nodiscard]] float floor_size() const { return floorSize_; }
    [[nodiscard]] QVector3D home() const { return home_; }
    [[nodiscard]] QVector3D floor_centre() const {
        return {std::round(home_.x() / 1000.0f) * 1000.0f, 0.0f,
                std::round(home_.z() / 1000.0f) * 1000.0f};
    }
    [[nodiscard]] float extent() const { return extent_; }
    [[nodiscard]] bool has_room() const { return hasRoom_; }
    [[nodiscard]] QString frame_camera() const { return frameCamera_; }
    [[nodiscard]] QVariantList axes() const;

    // Room frame (mm) to scene (mm, Y-up, floor at 0), and a room frame
    // direction to the scene's.
    [[nodiscard]] QVector3D to_scene(const Skeleton3DVec3& room) const;
    [[nodiscard]] QVector3D dir_to_scene(const QVector3D& roomDir) const;

   signals:
    void roomChanged();
    void saveImageRequested();

   private:
    QVector3D up_{0, -1, 0}, fwd_{0, 0, 1}, right_{1, 0, 0};
    float floorH_ = 0.0f;
    QVector3D home_{0, 1000, 0};
    float extent_    = 2000.0f;
    float floorSize_ = 6000.0f;
    bool hasRoom_    = false;
    QString frameCamera_;
    QVariantList cameras_;
};

// Small helpers for building the meshes and the page's lists.
namespace room3d {

inline QVector3D qv(const Skeleton3DVec3& p) {
    return {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
}

// One vertex, x y z r g b a, onto a mesh.
inline void put(std::vector<float>& v, const QVector3D& p, const QColor& c) {
    v.insert(v.end(),
             {p.x(), p.y(), p.z(), static_cast<float>(c.redF()), static_cast<float>(c.greenF()),
              static_cast<float>(c.blueF()), static_cast<float>(c.alphaF())});
}

inline QColor alpha(QColor c, double a) {
    c.setAlphaF(static_cast<float>(std::clamp(a, 0.0, 1.0)));
    return c;
}

} // namespace room3d

} // namespace mosaic
