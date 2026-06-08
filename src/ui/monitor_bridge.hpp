#pragma once
#include "core/settings.hpp"
#include "record/record_manager.hpp"
#include <QObject>
#include <QString>

namespace mosaic {

// QObject registered as QML context property "backend".
// Exposes RecordManager state to the QML monitoring view via Q_PROPERTYs
// and lets QML trigger recording via Q_INVOKABLEs.
//
// All properties notify QML automatically — no polling needed.

class MonitorBridge : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool    recording    READ isRecording  NOTIFY recordingChanged)
    Q_PROPERTY(int     elapsedMs    READ elapsedMs    NOTIFY elapsedMsChanged)
    Q_PROPERTY(int     cameraCount  READ cameraCount  NOTIFY cameraCountChanged)
    Q_PROPERTY(QString sessionPath  READ sessionPath  NOTIFY sessionPathChanged)

public:
    explicit MonitorBridge(RecordManager*      recordMgr,
                            const VideoSettings& videoSettings,
                            QObject*             parent = nullptr);

    // Q_PROPERTY readers
    [[nodiscard]] bool    isRecording() const;
    [[nodiscard]] int     elapsedMs()   const;
    [[nodiscard]] int     cameraCount() const;
    [[nodiscard]] QString sessionPath() const;

    // Called by VideoSettingsW when cameras are added/removed
    void set_camera_count(int count);

public slots:
    // Callable from QML: backend.startRecording() / backend.stopRecording()
    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();

signals:
    void recordingChanged();
    void elapsedMsChanged();
    void cameraCountChanged();
    void sessionPathChanged();

private:
    RecordManager*       m_rm;
    const VideoSettings& m_videoSettings;
    int                  m_cameraCount{0};
    QString              m_sessionPath;
};

} // namespace mosaic
