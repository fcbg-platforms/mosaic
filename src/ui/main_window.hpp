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

    // Aborts a pending pre-recording countdown, if one is running; a no-op
    // otherwise. Exists so Application's StopRecording *trigger* handler can
    // cancel a countdown: that handler only sees RecordManager, whose
    // is_recording() is still false during the countdown, so without this a
    // stop trigger fired in that window would be silently dropped and the
    // recording would start anyway a second or two later.
    void cancel_pending_recording_start();

   signals:
    // Emitted when the user chooses File → Switch profile.
    void switch_profile_requested();

   protected:
    void closeEvent(QCloseEvent* event) override;

   private:
    void build_menu_bar();
    void build_central_widget();
    void build_status_bar();

    // Gathers each configured camera's VideoManager::camera_stats() +
    // action_ticks_fired() + a SyncManifest lookup into a SessionHealthReport
    // and shows it in a new, non-modal SessionHealthDialog. Called right
    // after RecordManager::recording_stopped fires — see build_status_bar().
    void show_session_health(const QString& sessionPath, int durationMs);

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
