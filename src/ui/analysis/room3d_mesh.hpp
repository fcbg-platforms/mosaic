#pragma once
#include <QQuick3DGeometry>
#include <QString>

#include "ui/analysis/room3d_stage.hpp"

namespace mosaic {

// A mesh of a room page (Room3DMeshSource::mesh: the stage's grid and
// camera pyramids, a scene's people), as a Qt Quick 3D geometry:
// `SceneMesh { scene: room3d; part: "trails" }` in the page. Rebuilt when
// its source says that part changed.
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
    Room3DMeshSource* scene_ = nullptr;
    QString part_;
};

} // namespace mosaic
