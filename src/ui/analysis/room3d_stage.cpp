#include "ui/analysis/room3d_stage.hpp"

#include <QQuaternion>
#include <QVariantMap>
#include <QVector2D>

#include "analysis/room_view_math.hpp"
#include "video/camera_label.hpp"

namespace mosaic {

using room3d::alpha;
using room3d::put;
using room3d::qv;

namespace {

const QColor kCameraColors[] = {
    QColor("#44cc44"), QColor("#4488ff"), QColor("#ffaa44"),
    QColor("#ff4488"), QColor("#44cccc"), QColor("#cc44cc"),
};
constexpr float kGridStepMm = 500.0f;

QVector3D column(const std::array<double, 9>& m, int j) {
    return {static_cast<float>(m[j]), static_cast<float>(m[3 + j]), static_cast<float>(m[6 + j])};
}

QColor camera_color(int index) { return kCameraColors[((index % 6) + 6) % 6]; }

} // namespace

Room3DStage::Basis Room3DStage::basis_of(const Skeleton3DResult& result) {
    const auto up = room_view::estimate_up(result);
    return {qv(up), qv(room_view::floor_forward(up)),
            static_cast<float>(room_view::floor_height(result, up))};
}

QVector3D Room3DStage::to_scene(const Skeleton3DVec3& room) const {
    const QVector3D v = qv(room);
    return {QVector3D::dotProduct(v, right_), QVector3D::dotProduct(v, up_) - floorH_,
            -QVector3D::dotProduct(v, fwd_)};
}

QVector3D Room3DStage::dir_to_scene(const QVector3D& d) const {
    return {QVector3D::dotProduct(d, right_), QVector3D::dotProduct(d, up_),
            -QVector3D::dotProduct(d, fwd_)};
}

QVariantList Room3DStage::axes() const {
    return {
        QVariantMap{{"name", "x"}, {"dir", dir_to_scene({1, 0, 0})}, {"color", QColor("#ff6666")}},
        QVariantMap{{"name", "y"}, {"dir", dir_to_scene({0, 1, 0})}, {"color", QColor("#66dd66")}},
        QVariantMap{{"name", "z"}, {"dir", dir_to_scene({0, 0, 1})}, {"color", QColor("#6699ff")}}};
}

void Room3DStage::clear_room() {
    hasRoom_ = false;
    cameras_.clear();
    set_mesh("grid", {});
    set_mesh("frusta", {});
    set_mesh("frustumFaces", {});
    emit roomChanged();
}

void Room3DStage::set_room(const Basis& basis, const QVector<Skeleton3DRoomCamera>& cameras,
                           const std::vector<Skeleton3DVec3>& points) {
    up_      = basis.up;
    fwd_     = basis.fwd;
    right_   = QVector3D::crossProduct(fwd_, up_).normalized();
    floorH_  = basis.floorH;
    hasRoom_ = true;

    frameCamera_ = "the reference camera";
    for (const auto& c : cameras) {
        if (room_view::norm(c.positionRoom) < 1.0 &&
            (!c.hasRotation ||
             std::abs(c.rotationRoom[0] + c.rotationRoom[4] + c.rotationRoom[8] - 3.0) < 1e-6)) {
            frameCamera_ = camera_label(c.index);
        }
    }

    // Where the people were (their mean, at 1 m), and how far the cameras
    // and most of the people reach.
    std::vector<QVector3D> pts;
    pts.reserve(points.size());
    QVector3D sum;
    for (const auto& p : points) {
        pts.push_back(to_scene(p));
        sum += pts.back();
    }
    if (pts.empty()) {
        for (const auto& c : cameras) {
            pts.push_back(to_scene(c.positionRoom));
            sum += pts.back();
        }
    }
    home_ = pts.empty() ? QVector3D(0, 1000, 0) : sum / static_cast<float>(pts.size());
    home_.setY(1000.0f);
    auto flat = [this](const QVector3D& p) {
        return QVector2D(p.x() - home_.x(), p.z() - home_.z()).length();
    };
    float r = 1500.0f;
    for (const auto& c : cameras) {
        r = std::max(r, flat(to_scene(c.positionRoom)));
    }
    if (!pts.empty()) {
        std::sort(pts.begin(), pts.end(),
                  [&](const QVector3D& a, const QVector3D& b) { return flat(a) < flat(b); });
        r = std::max(r, flat(pts[pts.size() * 95 / 100])); // strays do not count
    }
    extent_    = r;
    floorSize_ = 2.0f * (std::ceil((r + 1000.0f) / 1000.0f) * 1000.0f);

    // The floor grid: every 0.5 m, brighter each metre, on whole metres.
    Mesh grid;
    const float half = floorSize_ / 2.0f;
    const float cx   = floor_centre().x();
    const float cz   = floor_centre().z();
    for (float g = -half; g <= half + 1.0f; g += kGridStepMm) {
        const bool metre = std::fmod(std::abs(g), 1000.0f) < 1.0f;
        // Dark colours rather than transparency (lines ignore alpha); they
        // are linear light, brightened on the way to the screen.
        const QColor c = metre ? QColor(34, 38, 62) : QColor(18, 20, 34);
        put(grid.vertices, {cx + g, 1.0f, cz - half}, c);
        put(grid.vertices, {cx + g, 1.0f, cz + half}, c);
        put(grid.vertices, {cx - half, 1.0f, cz + g}, c);
        put(grid.vertices, {cx + half, 1.0f, cz + g}, c);
    }

    // The cameras: their bodies are posed in the page; what each sees is a
    // pyramid 0.45 m deep, outlined, with a translucent far face.
    cameras_.clear();
    Mesh frusta, faces;
    faces.triangles = true;
    for (const auto& c : cameras) {
        const QVector3D o = to_scene(c.positionRoom);
        QVariantMap cam{{"pos", o},
                        {"color", camera_color(c.index)},
                        {"label", camera_label(c.index)},
                        {"hasRotation", c.hasRotation},
                        {"fovY", c.fovDeg[1]}};
        if (c.hasRotation) {
            const QVector3D x = dir_to_scene(column(c.rotationRoom, 0));
            const QVector3D y = dir_to_scene(column(c.rotationRoom, 1));
            const QVector3D z = dir_to_scene(column(c.rotationRoom, 2));
            // A node looks down its -z with +y up: the camera's look is z,
            // its image-up is -y.
            cam["rot"]           = QQuaternion::fromAxes(x, -y, -z);
            const float depth    = 450.0f;
            const float tx       = depth * static_cast<float>(std::tan(c.fovDeg[0] * M_PI / 360.0));
            const float ty       = depth * static_cast<float>(std::tan(c.fovDeg[1] * M_PI / 360.0));
            const QVector3D k[4] = {
                o + z * depth - x * tx - y * ty, o + z * depth + x * tx - y * ty,
                o + z * depth + x * tx + y * ty, o + z * depth - x * tx + y * ty};
            const QColor col = camera_color(c.index);
            for (int i = 0; i < 4; ++i) {
                put(frusta.vertices, o, alpha(col, 0.9));
                put(frusta.vertices, k[i], alpha(col, 0.9));
                put(frusta.vertices, k[i], alpha(col, 0.9));
                put(frusta.vertices, k[(i + 1) % 4], alpha(col, 0.9));
            }
            for (const int i : {0, 1, 2, 0, 2, 3}) {
                put(faces.vertices, k[i], alpha(col, 0.18));
            }
        }
        cameras_ << cam;
    }
    set_mesh("grid", std::move(grid));
    set_mesh("frusta", std::move(frusta));
    set_mesh("frustumFaces", std::move(faces));
    emit roomChanged();
}

} // namespace mosaic
