#include "ui/analysis/gaze3d_view_w.hpp"

#include <QVBoxLayout>

#include "ui/analysis/gaze3d_scene.hpp"
#include "ui/analysis/room3d_host_w.hpp"

namespace mosaic {

struct Gaze3DViewW::Impl {
    Gaze3DScene scene;
    Room3DHostW* host = nullptr;
};

Gaze3DViewW::Gaze3DViewW(QWidget* parent) : QWidget(parent), d(std::make_unique<Impl>()) {
    setMinimumSize(240, 180);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    // Save image names the file after the tick on show.
    d->host =
        new Room3DHostW("qrc:/qml/Gaze3D.qml", "gaze3d", &d->scene, &d->scene.stage(), [this] {
            return d->scene.tick() >= 0 ? QString("gaze3d_tick%1.png").arg(d->scene.tick())
                                        : QString("gaze3d.png");
        });
    lay->addWidget(d->host);
}

Gaze3DViewW::~Gaze3DViewW() {
    // The page reads the scene: it goes first.
    delete d->host;
}

void Gaze3DViewW::set_result(const GazeFusionResult& result, const Skeleton3DResult* pose) {
    d->scene.set_result(result, pose);
}

void Gaze3DViewW::set_position_ms(int64_t positionMs) { d->scene.set_position_ms(positionMs); }

void Gaze3DViewW::set_image_folder(const QString& folder) { d->host->set_image_folder(folder); }

} // namespace mosaic
