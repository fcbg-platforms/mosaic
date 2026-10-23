#pragma once
#include <QQuick3DGeometry>
#include <QString>

#include "ui/analysis/room3d_scene.hpp"

namespace mosaic {

// A mesh of the scene (Room3DScene::mesh), as a Qt Quick 3D geometry:
// `SceneMesh { scene: room3d; part: "trails" }` in the page. It follows the
// scene: rebuilt whenever the frame or the result changes.
class SceneMesh : public QQuick3DGeometry {
    Q_OBJECT
    Q_PROPERTY(QObject* scene READ scene WRITE set_scene NOTIFY sceneChanged)
    Q_PROPERTY(QString part READ part WRITE set_part NOTIFY partChanged)

   public:
    explicit SceneMesh(QQuick3DObject* parent = nullptr);
    [[nodiscard]] QObject* scene() const { return scene_; }
    [[nodiscard]] QString part() const { return part_; }
    void set_scene(QObject* scene);
    void set_part(const QString& part);

   signals:
    void sceneChanged();
    void partChanged();

   private:
    void rebuild();
    Room3DScene* scene_ = nullptr;
    QString part_;
};

} // namespace mosaic
