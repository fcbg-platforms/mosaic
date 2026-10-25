#include "ui/analysis/room3d_host_w.hpp"

#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QQmlContext>
#include <QQuickWidget>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtQml>

#include "ui/analysis/room3d_mesh.hpp"
#include "ui/analysis/room3d_stage.hpp"
#include "utils/logger.hpp"

namespace mosaic {

Room3DHostW::Room3DHostW(const QString& pageUrl, const QString& contextName, QObject* scene,
                         Room3DStage* stage, std::function<QString()> imageName, QWidget* parent)
    : QWidget(parent) {
    setMinimumSize(240, 180);
    // The pages' own type, once per process.
    static const int registered = qmlRegisterType<SceneMesh>("Mosaic.Room3D", 1, 0, "SceneMesh");
    Q_UNUSED(registered);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    quick_ = new QQuickWidget(this);
    quick_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick_->setClearColor(QColor("#0b0d16"));
    quick_->rootContext()->setContextProperty(contextName, scene);
    quick_->setSource(QUrl(pageUrl));
    if (quick_->status() == QQuickWidget::Error) {
        // Most likely Qt Quick 3D is missing next to MOSAIC (windeployqt not
        // run since it was added): say so, rather than show a blank view.
        QStringList errors;
        for (const auto& e : quick_->errors()) {
            errors << e.toString();
        }
        log_error("[Room3DHostW] " + pageUrl + " could not load: " + errors.join("; "));
        delete quick_;
        quick_     = nullptr;
        auto* note = new QLabel(
            "The 3D room view could not load (is Qt Quick 3D next to MOSAIC?). Help > Problems "
            "on this PC has the error.");
        note->setWordWrap(true);
        note->setAlignment(Qt::AlignCenter);
        lay->addWidget(note);
        return;
    }
    lay->addWidget(quick_);

    // Save image: the view as drawn, at its size on screen. Queued: the
    // dialog runs its own event loop, better not inside the page's mouse
    // handling.
    connect(
        stage, &Room3DStage::saveImageRequested, this,
        [this, imageName] {
            const QString folder =
                imageFolder_.isEmpty()
                    ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                    : imageFolder_;
            const QString path = QFileDialog::getSaveFileName(
                this, "Save the 3D view", folder + "/" + imageName(), "PNG image (*.png)");
            if (path.isEmpty() || !quick_) {
                return;
            }
            if (!quick_->grabFramebuffer().save(path, "PNG")) {
                QMessageBox::warning(this, "Save the 3D view", "Could not save " + path + ".");
            }
        },
        Qt::QueuedConnection);
}

Room3DHostW::~Room3DHostW() {
    // The page reads the scene: it goes before anything else.
    delete quick_;
}

} // namespace mosaic
