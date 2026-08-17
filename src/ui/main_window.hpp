#pragma once
#include <QMainWindow>
#include <memory>

#include "analysis/analysis_manager.hpp"
#include "audio/audio_manager.hpp"
#include "core/settings.hpp"
#include "record/record_manager.hpp"
#include "trigger/trigger_manager.hpp"
#include "video/video_manager.hpp"

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

class MainWindow : public QMainWindow {
    Q_OBJECT
   public:
    // isAdmin/otherUserDirectories implement per-user recording access
    // control (item 27): a non-admin's session browser/Analysis
    // tab/Record-settings directory field are scoped to just their own
    // settings.record.directory (unchanged default behavior,
    // otherUserDirectories left empty); an admin's session browser/
    // Analysis tab additionally scan every entry in otherUserDirectories
    // (every other known profile's own recording folder, resolved once in
    // Application::initialize()) and their Record-settings directory field
    // stays fully editable.
    explicit MainWindow(AppSettings& settings, const QString& username, TriggerManager* triggerMgr,
                        AudioManager* audioMgr, VideoManager* videoMgr, RecordManager* recordMgr,
                        AnalysisManager* analysisMgr, bool isAdmin = false,
                        const QStringList& otherUserDirectories = {}, QWidget* parent = nullptr);
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
