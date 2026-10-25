#include "ui/analysis/skeleton3d_room_view_w.hpp"

#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QQmlContext>
#include <QQuickWidget>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtQml>

#include "ui/analysis/room3d_mesh.hpp"
#include "ui/analysis/room3d_scene.hpp"
#include "utils/logger.hpp"

namespace mosaic {

struct Skeleton3DRoomViewW::Impl {
    Room3DScene scene;
    QQuickWidget* quick = nullptr;
    QString imageFolder;
};

Skeleton3DRoomViewW::Skeleton3DRoomViewW(QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>()) {
    setMinimumSize(240, 180);
    // The page's own type, once per process.
    static const int registered = qmlRegisterType<SceneMesh>("Mosaic.Room3D", 1, 0, "SceneMesh");
    Q_UNUSED(registered);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    d->quick = new QQuickWidget(this);
    d->quick->setResizeMode(QQuickWidget::SizeRootObjectToView);
    d->quick->setClearColor(QColor("#0b0d16"));
    d->quick->rootContext()->setContextProperty("room3d", &d->scene);
    d->quick->setSource(QUrl("qrc:/qml/Room3D.qml"));
    if (d->quick->status() == QQuickWidget::Error) {
        // Most likely Qt Quick 3D is missing next to MOSAIC (windeployqt not
        // run since it was added): say so, rather than show a blank view.
        QStringList errors;
        for (const auto& e : d->quick->errors()) {
            errors << e.toString();
        }
        log_error("[Skeleton3DRoomViewW] The 3D room view could not load: " + errors.join("; "));
        delete d->quick;
        d->quick   = nullptr;
        auto* note = new QLabel(
            "The 3D room view could not load (is Qt Quick 3D next to MOSAIC?). Help > Problems "
            "on this PC has the error.");
        note->setWordWrap(true);
        note->setAlignment(Qt::AlignCenter);
        lay->addWidget(note);
        return;
    }
    lay->addWidget(d->quick);

    // Save image: the view as drawn, at its size on screen, named after the
    // tick on show. Queued: the dialog runs its own event loop, better not
    // inside the page's mouse handling.
    connect(
        &d->scene, &Room3DScene::saveImageRequested, this,
        [this] {
            const QString folder =
                d->imageFolder.isEmpty()
                    ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                    : d->imageFolder;
            const QString name = d->scene.tick() >= 0
                                     ? QString("/room3d_tick%1.png").arg(d->scene.tick())
                                     : QString("/room3d.png");
            const QString path = QFileDialog::getSaveFileName(this, "Save the 3D view",
                                                              folder + name, "PNG image (*.png)");
            if (path.isEmpty() || !d->quick) {
                return;
            }
            if (!d->quick->grabFramebuffer().save(path, "PNG")) {
                QMessageBox::warning(this, "Save the 3D view", "Could not save " + path + ".");
            }
        },
        Qt::QueuedConnection);
}

Skeleton3DRoomViewW::~Skeleton3DRoomViewW() {
    // The page reads the scene: it goes first.
    delete d->quick;
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

void Skeleton3DRoomViewW::set_image_folder(const QString& folder) { d->imageFolder = folder; }

} // namespace mosaic
