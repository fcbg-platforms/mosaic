#pragma once
#include <QString>
#include <QWidget>
#include <functional>

class QQuickWidget;

namespace mosaic {

class Room3DStage;

// The widget around a 3D room page (the 3D Pose room view, the 3D Gaze
// view): the page in a QQuickWidget, given its scene under `contextName`;
// a note instead when the page cannot load (most likely Qt Quick 3D missing
// next to MOSAIC); and the page's Save image.
//
// The scene is the owner's and must outlive the page: the owner deletes
// this widget first in its destructor.
class Room3DHostW : public QWidget {
   public:
    // `imageName`: the file name Save image suggests (the tick on show).
    Room3DHostW(const QString& pageUrl, const QString& contextName, QObject* scene,
                Room3DStage* stage, std::function<QString()> imageName, QWidget* parent = nullptr);
    ~Room3DHostW() override;

    // Where Save image suggests saving (the session's folder); empty for the
    // user's Pictures folder.
    void set_image_folder(const QString& folder) { imageFolder_ = folder; }

   private:
    QQuickWidget* quick_ = nullptr;
    QString imageFolder_;
};

} // namespace mosaic
