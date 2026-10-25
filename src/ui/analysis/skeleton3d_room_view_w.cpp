#include "ui/analysis/skeleton3d_room_view_w.hpp"

#include <QVBoxLayout>

#include "ui/analysis/room3d_host_w.hpp"
#include "ui/analysis/room3d_scene.hpp"

namespace mosaic {

struct Skeleton3DRoomViewW::Impl {
    Room3DScene scene;
    Room3DHostW* host = nullptr;
};

Skeleton3DRoomViewW::Skeleton3DRoomViewW(QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>()) {
    setMinimumSize(240, 180);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    // Save image names the file after the tick on show.
    d->host =
        new Room3DHostW("qrc:/qml/Room3D.qml", "room3d", &d->scene, &d->scene.stage(), [this] {
            return d->scene.tick() >= 0 ? QString("room3d_tick%1.png").arg(d->scene.tick())
                                        : QString("room3d.png");
        });
    lay->addWidget(d->host);
}

Skeleton3DRoomViewW::~Skeleton3DRoomViewW() {
    // The page reads the scene: it goes first.
    delete d->host;
}

void Skeleton3DRoomViewW::set_result(const Skeleton3DResult& result) {
    d->scene.set_result(result);
}

void Skeleton3DRoomViewW::set_position_ms(int64_t positionMs) {
    d->scene.set_position_ms(positionMs);
}

void Skeleton3DRoomViewW::set_show_smoothed(bool showSmoothed) {
    d->scene.set_show_smoothed(showSmoothed);
}

void Skeleton3DRoomViewW::set_dyad_tracks(int personA, int personB) {
    d->scene.set_dyad(personA, personB);
}

void Skeleton3DRoomViewW::set_image_folder(const QString& folder) {
    d->host->set_image_folder(folder);
}

} // namespace mosaic
