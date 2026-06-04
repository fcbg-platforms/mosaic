#pragma once
#include "audio/audio_manager.hpp"
#include "core/settings.hpp"
#include "record/record_manager.hpp"
#include "trigger/trigger_manager.hpp"
#include "video/video_manager.hpp"
#include <QMainWindow>
#include <memory>

namespace mosaic {

// The application's single main window.
//
// Layout (hybrid Qt Widgets + QML):
//   ┌──────────────────────┬──────────────────────────────────────┐
//   │  Settings sidebar    │  QML monitoring view (live feeds,    │
//   │  (Qt Widgets)        │  waveforms, REC timer)               │
//   │  ├─ Video            │                                      │
//   │  ├─ Audio            │  MonitorView.qml                     │
//   │  ├─ Triggers         │                                      │
//   │  └─ Record           │                                      │
//   └──────────────────────┴──────────────────────────────────────┘
//
// The QML view communicates with C++ through a MonitorBridge QObject
// that will be registered as a QML context property (added later).

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(AppSettings&    settings,
                        const QString&  username,
                        TriggerManager* triggerMgr,
                        AudioManager*   audioMgr,
                        VideoManager*   videoMgr,
                        RecordManager*  recordMgr,
                        QWidget*        parent = nullptr);
    ~MainWindow() override;

signals:
    // Emitted when the user chooses File → Switch profile.
    void switch_profile_requested();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void build_menu_bar();
    void build_central_widget();
    void build_status_bar();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
