#include "ui/analysis/room3d_mesh.hpp"

#include <algorithm>

#include "ui/analysis/room3d_scene.hpp"

namespace mosaic {

// ── SceneMesh ────────────────────────────────────────────────────────────

SceneMesh::SceneMesh(QQuick3DObject* parent) : QQuick3DGeometry(parent) {}

void SceneMesh::set_scene(QObject* scene) {
    if (scene_) {
        disconnect(scene_, nullptr, this, nullptr);
    }
    scene_ = qobject_cast<Room3DScene*>(scene);
    if (scene_) {
        // The grid and the camera pyramids only change with the result.
        connect(scene_, &Room3DScene::frameChanged, this, [this] {
            if (part_ != "grid" && part_ != "frusta" && part_ != "frustumFaces") {
                rebuild();
            }
        });
        connect(scene_, &Room3DScene::resultChanged, this, &SceneMesh::rebuild);
    }
    emit sceneChanged();
    rebuild();
}

void SceneMesh::set_part(const QString& part) {
    part_ = part;
    emit partChanged();
    rebuild();
}

void SceneMesh::rebuild() {
    clear();
    if (!scene_ || part_.isEmpty()) {
        update();
        return;
    }
    const auto& m = scene_->mesh(part_);
    const int n   = static_cast<int>(m.vertices.size() / 7);
    QVector3D lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < n; ++i) {
        const QVector3D p(m.vertices[7 * i], m.vertices[7 * i + 1], m.vertices[7 * i + 2]);
        lo = QVector3D(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
        hi = QVector3D(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
    }
    setVertexData(QByteArray(reinterpret_cast<const char*>(m.vertices.data()),
                             static_cast<qsizetype>(m.vertices.size() * sizeof(float))));
    setStride(7 * sizeof(float));
    setPrimitiveType(m.triangles ? PrimitiveType::Triangles : PrimitiveType::Lines);
    addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
    addAttribute(Attribute::ColorSemantic, 3 * sizeof(float), Attribute::F32Type);
    if (n > 0) {
        setBounds(lo, hi);
    }
    update();
}

} // namespace mosaic
