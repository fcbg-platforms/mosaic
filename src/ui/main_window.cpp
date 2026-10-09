#include "ui/main_window.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QQmlContext>
#include <QQuickWidget>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <memory>

#include "analysis/pose_worker.hpp"
#include "analysis/sync_manifest.hpp"
#include "analysis/transcript_worker.hpp"
#include "session/session_health.hpp"
#include "ui/analysis/analysis_tab_w.hpp"
#include "ui/audio/audio_settings_w.hpp"
#include "ui/calibration/calibration_w.hpp"
#include "ui/help_dialog.hpp"
#include "ui/logger/logger_panel_w.hpp"
#include "ui/monitor_bridge.hpp"
#include "ui/realtime/realtime_tab_w.hpp"
#include "ui/record/record_settings_w.hpp"
#include "ui/session/session_browser_w.hpp"
#include "ui/session/session_health_dialog.hpp"
#include "ui/session/session_identity_dialog.hpp"
#include "ui/trigger/trigger_event_panel_w.hpp"
#include "ui/trigger/trigger_settings_w.hpp"
#include "ui/video/performance_monitor_w.hpp"
#include "ui/video/video_settings_w.hpp"
#include "utils/logger.hpp"
#include "utils/timestamp.hpp"
#include "video/camera_label.hpp"
#include "video/video_feed_provider.hpp"

namespace mosaic {

// Multi-base search for a file relative to the app, mirroring
// AnalysisManager::find_venv_python()'s already-proven approach — a naive
// single applicationDirPath()-relative guess silently fails in a dev build
// (built to build/Release/bin/Release/, while python/'s real venv lives in
// the source tree and is never copied next to the exe) with no diagnostic
// at all. Checking the working directory too (typical when launched from a
// shell/IDE with cwd at the source root) makes this resolve correctly here,
// exactly like the analysis/ Python plugins already do.
//
// Two different "up N levels" depths, both real: 4 levels matches
// scripts/configure.ps1's own documented -B build/$BuildType convention
// (build/<Cfg>/bin/<Cfg>/mosaic.exe); 3 levels matches ci.yml's plain
// -B build convention (build/bin/<Cfg>/mosaic.exe) — see
// AnalysisManager::find_venv_python()'s doc comment for the full story of
// how the 3-level case went unnoticed until a CI-only test caught it.
static QString find_relative_to_app(const QString& relPath) {
    const QStringList bases = {
        QCoreApplication::applicationDirPath(),
        QCoreApplication::applicationDirPath() + "/../../../..", // Xcode bundle / configure.ps1
        QCoreApplication::applicationDirPath() + "/../../..",    // ci.yml's `-B build`
        QDir::currentPath(),
    };
    for (const QString& base : bases) {
        const QString candidate = QDir(base).filePath(relPath);
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

struct MainWindow::Impl {
    AppSettings& settings;
    QString username;
    // Per-user recording access control (item 27) — resolved once in
    // Application::initialize(), read-only from here on.
    bool isAdmin = false;
    QStringList otherUserDirectories;
    TriggerManager* triggerMgr   = nullptr;
    AudioManager* audioMgr       = nullptr;
    VideoManager* videoMgr       = nullptr;
    RecordManager* recordMgr     = nullptr;
    AnalysisManager* analysisMgr = nullptr;
    MonitorBridge* bridge        = nullptr;

    QTabWidget* topTabs                = nullptr;
    QSplitter* mainSplitter            = nullptr;
    QSplitter* rightSplitter           = nullptr;
    QTabWidget* settingsTabs           = nullptr;
    QQuickWidget* monitorView          = nullptr;
    LoggerPanelW* loggerPanel          = nullptr;
    PoseWorker* poseWorker             = nullptr;
    TranscriptWorker* transcriptWorker = nullptr;
    RealtimeTabW* realtimeTab          = nullptr;
    AnalysisTabW* analysisTab          = nullptr;
    VideoSettingsW* videoSettingsW     = nullptr;

    // Coalesces rapid-fire camera_params_changed emissions (e.g. dragging a
    // slider fires valueChanged on every intermediate tick) into a single
    // VideoManager::apply_live_params() call per camera, ~150ms after the
    // user stops changing that camera's controls, instead of one blocking
    // round-trip of GenICam node writes per tick.
    QTimer* liveApplyDebounce = nullptr;
    QSet<int> pendingLiveApplyIndices;

    // See reopen_cameras(). Pending: a reopen was asked for while it could not
    // run. In flight: between close() and the deferred open() completing.
    bool reopenPending  = false;
    bool reopenInFlight = false;

    explicit Impl(AppSettings& s, const QString& user, bool admin, const QStringList& otherDirs,
                  TriggerManager* tm, AudioManager* am, VideoManager* vm, RecordManager* rm,
                  AnalysisManager* anlm)
        : settings(s),
          username(user),
          isAdmin(admin),
          otherUserDirectories(otherDirs),
          triggerMgr(tm),
          audioMgr(am),
          videoMgr(vm),
          recordMgr(rm),
          analysisMgr(anlm) {}
};

MainWindow::MainWindow(AppSettings& settings, const QString& username, TriggerManager* triggerMgr,
                       AudioManager* audioMgr, VideoManager* videoMgr, RecordManager* recordMgr,
                       AnalysisManager* analysisMgr, bool isAdmin,
                       const QStringList& otherUserDirectories, QWidget* parent)
    : QMainWindow(parent),
      d(std::make_unique<Impl>(settings, username, isAdmin, otherUserDirectories, triggerMgr,
                               audioMgr, videoMgr, recordMgr, analysisMgr)) {
    const QString userLabel = (username == "guest") ? "Guest" : ("@" + username);
    setWindowTitle(QString("MOSAIC — %1").arg(userLabel));
    setMinimumSize(1200, 720);
    resize(1680, 960);

    QSettings prefs("CSRU", "MOSAIC");
    if (prefs.contains("mainWindow/geometry")) {
        restoreGeometry(prefs.value("mainWindow/geometry").toByteArray());
    }
    if (prefs.contains("mainWindow/state")) {
        restoreState(prefs.value("mainWindow/state").toByteArray());
    }

    build_menu_bar();
    build_central_widget();
    build_status_bar();
}

MainWindow::~MainWindow() = default;

void MainWindow::cancel_pending_recording_start() {
    if (d->bridge) d->bridge->cancel_countdown();
}

bool MainWindow::start_recording_from_trigger() {
    if (!d->bridge) return false;
    d->bridge->start_from_trigger();
    return true;
}

// ── Menu bar ───────────────────────────────────────────────────────────────

void MainWindow::build_menu_bar() {
    auto* file = menuBar()->addMenu("&File");

    auto* openAction = new QAction("&Open recordings folder…", this);
    openAction->setShortcut(QKeySequence("Ctrl+Shift+O"));
    connect(openAction, &QAction::triggered, this, [this] {
        const QString path =
            d->recordMgr ? d->recordMgr->current_session_path() : d->settings.record.directory;
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    file->addAction(openAction);

    auto* browseAction = new QAction("&Browse Sessions…", this);
    browseAction->setShortcut(QKeySequence("Ctrl+B"));
    connect(browseAction, &QAction::triggered, this, [this] {
        SessionBrowserW browser(d->settings.record.directory, d->analysisMgr,
                                d->isAdmin ? d->otherUserDirectories : QStringList{}, this);
        browser.exec();
    });
    file->addAction(browseAction);
    file->addSeparator();

    auto* switchAction = new QAction(QString("Switch profile  (current: %1)")
                                         .arg(d->username == "guest" ? "Guest" : "@" + d->username),
                                     this);
    switchAction->setShortcut(QKeySequence("Ctrl+Shift+P"));
    connect(switchAction, &QAction::triggered, this, [this] {
        if (d->recordMgr && d->recordMgr->is_recording()) {
            d->recordMgr->stop();
        }
        // Exit code 42 signals main() to re-show the login dialog
        // rather than quit entirely.
        QCoreApplication::exit(42);
    });
    file->addAction(switchAction);
    file->addSeparator();

    auto* quitAction = new QAction("&Quit", this);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, QCoreApplication::instance(), &QCoreApplication::quit);
    file->addAction(quitAction);

    auto* view = menuBar()->addMenu("&View");
    view->addAction("Reset layout", this, [this] {
        d->mainSplitter->setSizes({360, width() - 360});
        // mainSplitter now lives inside topTabs' "Live" tab page, so its
        // available height is the window height minus the tab bar itself.
        const int tabBarH = d->topTabs ? d->topTabs->tabBar()->height() : 0;
        d->rightSplitter->setSizes({height() - tabBarH - 120, 120});
        if (d->realtimeTab) {
            d->realtimeTab->reset_layout();
        }
    });
    view->addSeparator();

    auto* logToggle = new QAction("Show &Log Panel", this);
    logToggle->setShortcut(QKeySequence("Ctrl+L"));
    logToggle->setCheckable(true);
    logToggle->setChecked(true);
    connect(logToggle, &QAction::toggled, this, [this](bool shown) {
        if (d->loggerPanel) {
            d->loggerPanel->setVisible(shown);
        }
    });
    view->addAction(logToggle);

    auto* record      = menuBar()->addMenu("&Record");
    auto* startAction = new QAction("▶  Start recording", this);
    startAction->setShortcut(QKeySequence("Ctrl+R"));
    connect(startAction, &QAction::triggered, this, [this] {
        if (d->bridge) {
            d->bridge->startRecording();
        }
    });
    record->addAction(startAction);

    auto* stopAction = new QAction("■  Stop recording", this);
    stopAction->setShortcut(QKeySequence("Ctrl+."));
    connect(stopAction, &QAction::triggered, this, [this] {
        if (d->bridge) {
            d->bridge->stopRecording();
        }
    });
    record->addAction(stopAction);

    auto* help = menuBar()->addMenu("&Help");
    help->addAction("&About MOSAIC…", this, [this] {
        HelpDialog dlg(this);
        dlg.exec();
    });
}

// ── Central widget ─────────────────────────────────────────────────────────

void MainWindow::build_central_widget() {
    // Top-level split between the live acquisition view and the post-hoc
    // Analysis tab — the only top-level QTabWidget; `settingsTabs` below is
    // a separate, narrower sidebar nested inside the "Live" tab's content.
    d->topTabs = new QTabWidget(this);
    d->topTabs->setDocumentMode(true);
    setCentralWidget(d->topTabs);

    // Outer horizontal split: settings sidebar | right pane
    d->mainSplitter = new QSplitter(Qt::Horizontal);
    d->mainSplitter->setHandleWidth(2);

    // ── Left: settings tabs ───────────────────────────────────────────────
    d->settingsTabs = new QTabWidget;
    d->settingsTabs->setMinimumWidth(300);
    d->settingsTabs->setMaximumWidth(520);
    d->settingsTabs->setDocumentMode(true);

    auto* videoSettingsW = new VideoSettingsW(d->settings.video);
    d->videoSettingsW    = videoSettingsW;
    d->settingsTabs->addTab(videoSettingsW, "Video");
    // Cameras are already opened and previewing by the time MainWindow is
    // constructed (Application::initialize() does this first) — start
    // "Discover cameras" disabled to match, rather than a moment of being
    // clickable before the recording_started/stopped guards in
    // build_status_bar() ever fire.
    videoSettingsW->set_discover_enabled(!(d->videoMgr && d->videoMgr->camera_count() > 0));
    d->settingsTabs->addTab(new AudioSettingsW(d->settings.audio, d->audioMgr), "Audio");
    d->settingsTabs->addTab(new TriggerSettingsW(d->settings.trigger, d->triggerMgr), "Triggers");
    d->settingsTabs->addTab(new TriggerEventPanelW(d->triggerMgr), "Events");
    auto* recordSettingsW = new RecordSettingsW(d->settings.record, d->isAdmin);
    d->settingsTabs->addTab(recordSettingsW, "Record");
    d->settingsTabs->addTab(new PerformanceMonitorW(d->videoMgr, d->audioMgr, d->analysisMgr),
                            "Perf");
    d->settingsTabs->addTab(new CalibrationW(d->settings.video, d->settings.room, d->videoMgr),
                            "Calibrate");

    d->mainSplitter->addWidget(d->settingsTabs);

    // ── Right: vertical split — monitor view on top, log panel on bottom ──
    d->rightSplitter = new QSplitter(Qt::Vertical);
    d->rightSplitter->setHandleWidth(3);

    // QML monitoring view
    d->monitorView = new QQuickWidget;
    d->monitorView->setResizeMode(QQuickWidget::SizeRootObjectToView);
    d->monitorView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    d->bridge = new MonitorBridge(d->recordMgr, d->settings.video, d->settings.record, this);

    // The record settings the monitor mirrors (start countdown, hide-previews)
    // must reach the live QML view the moment they're edited, not only after
    // the next app start — settings themselves are persisted separately, by
    // Application::shutdown().
    connect(recordSettingsW, &RecordSettingsW::settings_changed, d->bridge,
            &MonitorBridge::refresh_record_settings);
    d->bridge->set_preflight_provider([this] { return run_preflight(); });

    // Session-details prompt. Owned here rather than in MonitorBridge because
    // the bridge is a plain QObject with no widget parent — pulling QtWidgets
    // into it would destroy the separation it exists to maintain — and because
    // a QML popup is not usable for this: the project targets Qt 6.4, where a
    // Popup is clipped to its QQuickWidget, and that widget is a leaf of a
    // splitter the user can drag arbitrarily small.
    //
    // Queued, not direct. exec() spins a nested event loop, and a direct
    // connection would enter it from inside QQuickWidget's delivery of the
    // record button's own mouse event. Queuing lets startRecording() return to
    // QML first, then opens the dialog.
    connect(
        d->bridge, &MonitorBridge::identityConfirmationNeeded, this,
        [this](const QString& subject, const QString& session, const QString& task) {
            QString chosenSubject;
            QString chosenSession;
            QString chosenTask;
            const bool accepted = [&] {
                // Scoped so the dialog is destroyed before the bridge is called
                // below. With a zero start delay, arm_countdown() goes straight
                // into RecordManager::start(), which blocks the GUI thread for
                // several hundred ms — leaving this window on screen across it
                // would read as a freeze.
                SessionIdentityDialog dlg(
                    subject, session, task,
                    [this](const SessionIdentity& id) {
                        const auto& rec = d->settings.record;
                        return advise_identity(id, existing_session_names(rec.directory),
                                               rec.directory, QDateTime::currentDateTime(),
                                               rec.addTimestamp ? rec.timestampFormat : QString());
                    },
                    d->bridge->last_preflight(), this);
                if (dlg.exec() != QDialog::Accepted) {
                    return false;
                }
                chosenSubject = dlg.subject();
                chosenSession = dlg.session();
                chosenTask    = dlg.task();
                return true;
            }();

            if (!accepted) {
                d->bridge->cancelPendingStart();
                return;
            }
            if (!d->bridge->confirmIdentityAndStart(chosenSubject, chosenSession, chosenTask)) {
                // A trigger started a recording while the dialog was open. The
                // screen already shows REC, so without this the operator would
                // reasonably read their own click as having started it — and
                // that running session carries a different identity.
                QMessageBox::information(
                    this, "Already recording",
                    "A recording was started by a trigger while you were entering the "
                    "session details.\n\nYour details were saved for the next recording; "
                    "the one now running is not named after them.");
            }
        },
        Qt::QueuedConnection);

    // Register the camera-frame image provider before loading QML.
    auto* feedProvider = new VideoFeedProvider; // owned by QML engine
    d->monitorView->engine()->addImageProvider("videofeed", feedProvider);
    d->bridge->set_feed_provider(feedProvider);

    d->monitorView->rootContext()->setContextProperty("backend", d->bridge);
    d->monitorView->setSource(QUrl("qrc:/qml/MonitorView.qml"));

    if (d->monitorView->status() == QQuickWidget::Error) {
        for (const auto& err : d->monitorView->errors()) {
            log_error(QString("QML error: %1").arg(err.toString()));
        }
    }

    // Wire preview frames from VideoManager to MonitorBridge → image provider.
    if (d->videoMgr) {
        connect(d->videoMgr, &VideoManager::frame_preview, d->bridge,
                &MonitorBridge::on_frame_preview, Qt::QueuedConnection);

        // Per-camera delivery health, polled into the monitor once a second.
        //
        // Polled because there is nothing periodic to subscribe to: VideoManager
        // emits per-event signals and one-shot measurements, but nothing
        // carrying a rate. camera_stats() is what PerformanceMonitorW already
        // polls at this cadence, so the Live and Perf tabs cannot disagree.
        //
        // Over every *configured* camera by configured index — not
        // camera_count()/camera_stats(), which count and index only the cameras
        // that opened. With Camera 2 down, position 1 is Camera 3, and a chip
        // reporting its neighbour's rate would send the operator to the wrong
        // cable.
        auto* healthTimer = new QTimer(this);
        healthTimer->setInterval(1000);
        connect(healthTimer, &QTimer::timeout, this, [this] {
            if (!d->videoMgr || !d->bridge) return;
            const auto n = d->settings.video.cameras.size();
            std::vector<double> fps(n, 0.0);
            std::vector<bool> running(n, false);
            for (std::size_t i = 0; i < n; ++i) {
                const auto st = d->videoMgr->camera_stats_for_config_index(static_cast<int>(i));
                // Judged by its newest frame's age as well as its rate: the
                // grabber's rate is only recomputed on a good frame, so a
                // camera whose link has dropped keeps its last healthy figure.
                // Frame timestamps are on the elapsed_ns() clock.
                const double ageSec =
                    st.lastFrameElapsedNs >= 0
                        ? static_cast<double>(elapsed_ns() - st.lastFrameElapsedNs) / 1e9
                        : -1.0;
                fps[i]     = delivered_fps(st.fps, ageSec);
                running[i] = st.grabberRunning;
            }
            d->bridge->update_camera_health(fps, running);
        });
        healthTimer->start();
        // Live per-camera "does this firmware support GigE Vision Action
        // Command triggering" readout — see gige_action_command.hpp /
        // CameraCardW::set_action_command_capability().
        connect(d->videoMgr, &VideoManager::action_command_capability, videoSettingsW,
                &VideoSettingsW::set_action_command_capability);

        // Live per-camera achievable-frame-rate readout beside the exposure
        // controls — the camera's own ResultingFrameRate, which accounts for
        // exposure, sensor readout and GigE bandwidth together, rather than a
        // client-side 1/exposure guess. See compute_fps_readout().
        connect(d->videoMgr, &VideoManager::achievable_fps_changed, videoSettingsW,
                &VideoSettingsW::set_achievable_fps);
        // The interview camera's real rate, for the monitor's badge — which
        // used to show the rate asked for (50) while the camera ran at 36.7.
        connect(d->videoMgr, &VideoManager::achievable_fps_changed, this,
                [this](int configIndex, double fps) {
                    if (configIndex == d->videoMgr->interview_camera_index()) {
                        d->bridge->set_interview_rates(d->videoMgr->camera_max_fps(configIndex),
                                                       fps);
                    }
                });
        publish_interview_rates(); // the cameras were opened before this window
    }

    // When the camera list changes, fully reload the hardware.
    //
    // Sequence:
    //   1. set_camera_count(0) — QML immediately hides every camera delegate.
    //      This must happen BEFORE close() so the render thread never tries to
    //      sync a scenegraph that is removing a live, actively-rendering slot.
    //   2. close() — stops grabbers, closes Pylon devices, drains queued events.
    //   3. singleShot(0) — gives the event loop one pass so QML can fully process
    //      the count-0 change (delegates torn down cleanly) before open() blocks.
    //   4. open() + set_camera_count(opened) — bring up the new camera set and
    //      restore the slot count only after hardware is ready.
    connect(videoSettingsW, &VideoSettingsW::cameras_list_changed, this,
            [this] { reopen_cameras("camera list changed"); });

    // Interview mode: the header toggle and the Video tab's checkbox both ask,
    // MainWindow decides. Queued from the bridge so the reopen never runs
    // inside QML's delivery of the click that asked for it.
    connect(
        d->bridge, &MonitorBridge::interviewModeRequested, this,
        [this](bool on) { set_interview_mode(on); }, Qt::QueuedConnection);
    connect(videoSettingsW, &VideoSettingsW::interview_mode_requested, this,
            [this](bool on) { set_interview_mode(on); });
    // A new crop/rate/camera only needs the hardware touched if interview
    // mode is what is open; otherwise it simply waits for the next switch.
    connect(videoSettingsW, &VideoSettingsW::interview_settings_applied, this, [this] {
        if (d->settings.video.interview_active() ||
            (d->videoMgr && d->videoMgr->interview_camera_index() >= 0)) {
            reopen_cameras("interview settings applied");
        }
    });
    // A reopen deferred because Record had been clicked runs as soon as that
    // start is abandoned — not only once a recording that never began "stops".
    //
    // The interview section locks from the click, not from recording_started:
    // an Apply during the countdown would otherwise rewrite the settings that
    // session_meta.json, the health report and the master timeline are built
    // from, while the camera records with the old ones.
    connect(d->bridge, &MonitorBridge::startPendingChanged, this, [this] {
        if (d->videoSettingsW) {
            d->videoSettingsW->set_recording_locked(d->bridge->startPending() ||
                                                    (d->recordMgr && d->recordMgr->is_recording()));
        }
        if (d->reopenPending && !d->bridge->startPending() &&
            !(d->recordMgr && d->recordMgr->is_recording())) {
            QTimer::singleShot(0, this, [this] { reopen_cameras("deferred request"); });
        }
    });

    // A single camera's own parameters changed (exposure/gain/gamma/black
    // level/white balance/auto-target/digital shift, etc.) — push the
    // subset that's safe to change live straight to the open camera instead
    // of waiting for the next full close+reopen cycle. Structural changes
    // (resolution, pixel format, frame rate, HW trigger) are silently a
    // no-op here (VideoGrabber::apply_live_params only touches the safe
    // subset) and still require cameras_list_changed's reopen path.
    //
    // Debounced: a slider drag fires this once per intermediate tick, and
    // each call would otherwise mean a fresh round of GenICam node writes —
    // coalesce bursts for the same camera into one apply, shortly after the
    // user stops moving the control.
    d->liveApplyDebounce = new QTimer(this);
    d->liveApplyDebounce->setSingleShot(true);
    d->liveApplyDebounce->setInterval(150);
    connect(d->liveApplyDebounce, &QTimer::timeout, this, [this] {
        if (d->videoMgr) {
            const QSet<int> indices = d->pendingLiveApplyIndices;
            for (const int index : indices) {
                d->videoMgr->apply_live_params(index);
            }
        }
        d->pendingLiveApplyIndices.clear();
    });
    connect(videoSettingsW, &VideoSettingsW::camera_params_changed, this, [this](int index) {
        d->pendingLiveApplyIndices.insert(index);
        d->liveApplyDebounce->start();
    });

    // Start real-time pose worker if the Python venv is available. Uses a
    // multi-base search (find_relative_to_app(), above) rather than a
    // single applicationDirPath()-relative guess, since python/'s venv
    // lives in the source tree and nothing currently copies it next to a
    // built exe — see that function's own doc comment.
    {
#ifdef Q_OS_WIN
        const QString interpRel = "python/.venv/Scripts/python.exe";
#else
        const QString interpRel = "python/.venv/bin/python";
#endif
        const QString interp = find_relative_to_app(interpRel);
        const QString script = find_relative_to_app("python/pose/frame_server.py");
        if (interp.isEmpty() || script.isEmpty()) {
            log_warning(
                "Real-time pose/gaze unavailable — python/.venv or "
                "python/pose/frame_server.py not found next to the "
                "application or in the working directory. The "
                "Real-time tab will show no skeleton/gaze overlay "
                "until the python/ venv is set up and reachable.");
        } else {
            d->poseWorker = new PoseWorker(this);
            if (d->poseWorker->start(interp, script)) {
                if (d->videoMgr) {
                    // Send each enabled camera at ≤5 fps (200ms min interval).
                    // Sized against the lite model's real per-frame cost (see
                    // frame_server.py's explicit model_complexity=0), not the
                    // theoretical worst case: with only one camera's "Analyze"
                    // checkbox on at a time — today's typical config — this is
                    // comfortably within budget and makes the Real-time tab's
                    // Live Trace panel visibly smoother than the previous
                    // 2 fps. If several cameras ever have live analysis
                    // enabled simultaneously, the aggregate demand on the one
                    // shared MediaPipe subprocess (running pose + gaze per
                    // frame) can exceed its throughput and reintroduce
                    // queueing lag — lower this back down (or make it a
                    // proper per-camera/tunable setting) if that pattern
                    // becomes real usage, not just a hypothetical.
                    // Per-camera timestamps prevent one fast camera from starving others.
                    auto ts = std::make_shared<std::array<qint64, 16>>();
                    ts->fill(0);
                    connect(
                        d->videoMgr, &VideoManager::frame_preview, this,
                        [this, ts](int camIdx, QImage frame) {
                            if (!d->poseWorker || !d->poseWorker->is_running()) return;
                            if (camIdx < 0 || camIdx >= static_cast<int>(ts->size())) return;
                            // Per-camera opt-out (Real-time tab's "Analyze"
                            // checkbox) — separate from PoseWorker::set_paused()
                            // below: this is a per-camera user preference, that
                            // is a global recording-in-progress resource policy;
                            // neither should be able to stomp the other.
                            if (camIdx < static_cast<int>(d->settings.video.cameras.size()) &&
                                !d->settings.video.cameras[static_cast<size_t>(camIdx)]
                                     .liveAnalysisEnabled) {
                                return;
                            }
                            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                            if (nowMs - (*ts)[camIdx] < 200) return; // 5 fps per camera
                            (*ts)[camIdx] = nowMs;
                            d->poseWorker->submit_frame(camIdx, frame);
                        },
                        Qt::QueuedConnection);
                }
                if (d->recordMgr) {
                    // Auto-pause pose/gaze inference while recording is
                    // active, freeing CPU for the 6-camera grab/encode
                    // pipeline — a global resource policy, wired here (not
                    // inside RealtimeTabW) so it holds regardless of which
                    // tab is open. The subprocess itself is never torn
                    // down; it just idles.
                    connect(d->recordMgr, &RecordManager::recording_started, d->poseWorker,
                            [this](const QString&) { d->poseWorker->set_paused(true); });
                    connect(d->recordMgr, &RecordManager::recording_stopped, d->poseWorker,
                            [this](const QString&, int) { d->poseWorker->set_paused(false); });
                }
                log_info("Pose worker started — real-time pose overlay active.");
            } else {
                log_warning(
                    "Real-time pose worker failed to start (found "
                    "python.exe/frame_server.py, but launching the "
                    "process failed) — the Real-time tab will show "
                    "no skeleton/gaze overlay. Check mosaic.log above "
                    "this line for the underlying process error.");
            }
        }
    }

    // Start real-time transcript worker if the analysis/ venv is available.
    // Uses analysis/.venv (NOT python/.venv — see find_relative_to_app()'s
    // own doc comment and analysis/run_live_transcribe.py's module doc
    // comment for why: faster-whisper/torch already live in the
    // mosaic-analysis venv used by the post-hoc diarization pipeline,
    // avoiding duplicating that heavy dependency set into python/'s
    // otherwise-light mosaic-pose venv).
    {
#ifdef Q_OS_WIN
        const QString transcriptInterpRel = "analysis/.venv/Scripts/python.exe";
#else
        const QString transcriptInterpRel = "analysis/.venv/bin/python";
#endif
        const QString transcriptInterp = find_relative_to_app(transcriptInterpRel);
        const QString transcriptScript = find_relative_to_app("analysis/run_live_transcribe.py");
        if (transcriptInterp.isEmpty() || transcriptScript.isEmpty()) {
            log_warning(
                "Live transcript unavailable — analysis/.venv or "
                "analysis/run_live_transcribe.py not found next to the "
                "application or in the working directory. The "
                "Real-time tab's Transcript panel will show no live "
                "captions until the analysis/ venv is set up and reachable.");
        } else {
            d->transcriptWorker = new TranscriptWorker(this);
            if (d->transcriptWorker->start(transcriptInterp, transcriptScript,
                                           {"--model", "tiny"})) {
                if (d->audioMgr && !d->settings.audio.microphones.empty()) {
                    // Mic 0 only for v1 — see RealtimeTabW's own doc comment
                    // for the scope justification. AudioManager emits
                    // raw_pcm_ready regardless of monitor-only vs. real
                    // recording (matches VideoManager::frame_preview's
                    // always-on behavior feeding PoseWorker); pause is
                    // enforced below via set_paused(), not here.
                    connect(
                        d->audioMgr, &AudioManager::raw_pcm_ready, d->transcriptWorker,
                        [this](int micIdx, QByteArray pcm, int sr, int ch) {
                            if (micIdx != 0 || !d->transcriptWorker) return;
                            d->transcriptWorker->submit_chunk(0, sr, ch, pcm);
                        },
                        Qt::QueuedConnection);
                }
                if (d->recordMgr) {
                    // Same auto-pause reasoning/wiring as PoseWorker's block
                    // above — global resource policy, wired here not inside
                    // RealtimeTabW, holds regardless of which tab is open.
                    connect(d->recordMgr, &RecordManager::recording_started, d->transcriptWorker,
                            [this](const QString&) { d->transcriptWorker->set_paused(true); });
                    connect(
                        d->recordMgr, &RecordManager::recording_stopped, d->transcriptWorker,
                        [this](const QString&, int) { d->transcriptWorker->set_paused(false); });
                }
                log_info("Transcript worker started — live captions active.");
            } else {
                log_warning(
                    "Live transcript worker failed to start (found "
                    "python.exe/run_live_transcribe.py, but launching "
                    "the process failed) — the Real-time tab's "
                    "Transcript panel will show no live captions. "
                    "Check mosaic.log above this line for the "
                    "underlying process error.");
            }
        }
    }

    d->rightSplitter->addWidget(d->monitorView);

    // Logger panel
    d->loggerPanel = new LoggerPanelW;
    // Kept short so the video gets the height: a handful of recent lines is
    // what the panel is for at a glance, and it scrolls.
    d->loggerPanel->setMaximumHeight(160);
    d->loggerPanel->setMinimumHeight(60);
    d->rightSplitter->addWidget(d->loggerPanel);

    // A QML Layout.minimumHeight cannot hold a QSplitter back — the splitter
    // simply gives the QQuickWidget less than the minimums add up to, the
    // column overflows, and the Record button goes off the bottom edge with no
    // scrollbar to recover it. This is the guard that actually stops that, and
    // the reason it is safe: the window floor is 1200x720 and the logger panel
    // caps at 160 a few lines above, so 360 always fits.
    d->monitorView->setMinimumHeight(360);
    d->rightSplitter->setChildrenCollapsible(false);
    d->rightSplitter->setStretchFactor(0, 1);
    d->rightSplitter->setStretchFactor(1, 0);
    d->rightSplitter->setSizes({700, 110});

    d->mainSplitter->addWidget(d->rightSplitter);
    d->mainSplitter->setStretchFactor(0, 0);
    d->mainSplitter->setStretchFactor(1, 1);
    d->mainSplitter->setSizes({380, 1300});

    d->topTabs->addTab(d->mainSplitter, "Live");

    d->realtimeTab = new RealtimeTabW(d->settings, d->videoMgr, d->audioMgr, d->recordMgr,
                                      d->poseWorker, d->transcriptWorker);
    d->topTabs->addTab(d->realtimeTab, "Real-time");

    d->analysisTab = new AnalysisTabW(d->settings, d->analysisMgr,
                                      d->isAdmin ? d->otherUserDirectories : QStringList{});
    d->topTabs->addTab(d->analysisTab, "Analysis");
}

// ── Status bar ─────────────────────────────────────────────────────────────

void MainWindow::build_status_bar() {
    // No message label here by design — this used to surface the last
    // warning/error, recording state, and frame-drop notices as transient
    // text at the very bottom of the window; removed on request since that
    // text was distracting/unwanted. The LoggerPanelW dock (View → Logs)
    // remains the place to see warnings/errors. The side effects those
    // handlers also had (disabling "Discover cameras" while recording, in
    // particular) are preserved below — only the on-screen text is gone.

    // User chip in the right corner of the status bar.
    const QString userLabel =
        (d->username == "guest") ? "👤  Guest session" : QString("👤  @%1").arg(d->username);
    auto* userChip = new QLabel(userLabel);
    userChip->setStyleSheet(
        "QLabel { background: #12122a; border: 1px solid #252545; border-radius: 10px;"
        "  padding: 2px 10px; color: #7878a0; font-size: 11px; }");
    userChip->setToolTip("Current profile — use File → Switch profile to change");
    statusBar()->addPermanentWidget(userChip);

    if (d->recordMgr) {
        connect(d->recordMgr, &RecordManager::recording_started, this,
                [this](const QString& /*path*/) {
                    if (d->videoSettingsW) {
                        d->videoSettingsW->set_discover_enabled(false);
                        d->videoSettingsW->set_recording_locked(true);
                    }
                    // A session can end without VideoManager::stop() ever
                    // running (video disabled, or no camera open), which would
                    // otherwise leave the previous recording's snapshot in
                    // place for this session's health report to present as its
                    // own. Clearing here makes "stale" impossible rather than
                    // unlikely.
                    if (d->videoMgr) {
                        d->videoMgr->clear_recording_snapshot();
                    }
                });
        connect(d->recordMgr, &RecordManager::recording_stopped, this,
                [this](const QString& /*path*/, int /*durationMs*/) {
                    // Preview auto-resumes right after recording stops (see
                    // Application's own recording_stopped handler), keeping every
                    // Action1-armed camera's ticker alive — only re-enable if there
                    // are genuinely no cameras open at all.
                    if (d->videoSettingsW && d->videoMgr) {
                        d->videoSettingsW->set_discover_enabled(d->videoMgr->camera_count() == 0);
                    }
                    if (d->videoSettingsW) {
                        d->videoSettingsW->set_recording_locked(false);
                    }
                    // Deferred, not inline: Application's own handler restarts
                    // the preview, and show_session_health() reads this
                    // recording's snapshot — both must see the cameras that
                    // recorded, so the reopen runs once every handler is done.
                    if (d->reopenPending) {
                        QTimer::singleShot(0, this, [this] {
                            reopen_cameras("deferred until recording stopped");
                        });
                    }
                });
        connect(
            d->recordMgr, &RecordManager::recording_stopped, this,
            [this](const QString& path, int durationMs) { show_session_health(path, durationMs); });
    }
}

void MainWindow::show_session_health(const QString& sessionPath, int durationMs) {
    if (!d->videoMgr) {
        return;
    }

    // Cheap and synchronous per SyncManifest's own doc comment — generating
    // it here, right after every timestamps_camN.csv is finalized, also
    // leaves a fresh (not stale, see sync_manifest_is_stale()) manifest on
    // disk for later on-demand consumers (AnalysisManager, SessionPlayerW)
    // to reuse instead of redundantly regenerating it themselves.
    SyncManifest sync   = SyncManifest::generate(sessionPath);
    const bool haveSync = sync.is_valid();
    if (haveSync) {
        sync.save(sessionPath);
    }

    // Names a camera by its configured slot and serial rather than
    // friendlyName: Discover overwrites friendlyName with cam.modelName
    // (video_settings_w.cpp), which is identical across same-model units, so
    // every row would read the same. 1-based to match CameraCardW's own
    // headers; the artifacts it maps to are 0-based (Camera 1 -> video_0.mp4),
    // which the dialog spells out.
    const auto& configured  = d->settings.video.cameras;
    const auto camera_label = [&configured](int configIndex) {
        QString label = mosaic::camera_label(configIndex);
        if (configIndex >= 0 && configIndex < static_cast<int>(configured.size())) {
            const QString& serial = configured[static_cast<size_t>(configIndex)].serialNumber;
            if (!serial.isEmpty()) {
                label += QString(" (%1)").arg(serial);
            }
        }
        return label;
    };
    // CameraSync::index is the camera's real config index, so matching on it
    // below is sound even when cameras are missing. That was not always true:
    // SyncManifest used to scan cam0, cam1, … and stop at the first missing
    // file, numbering by position in that run, so a camera that failed to open
    // made every camera after it report "n/a" despite valid data on disk. It
    // now enumerates by camera number — see analysis/camera_timestamp_files.hpp.
    const auto fill_sync = [&](CameraHealthInput& in, int configIndex) {
        if (!haveSync) {
            return;
        }
        for (int c = 0; c < sync.camera_count(); ++c) {
            const auto& camSync = sync.camera_info(c);
            if (camSync.index == configIndex) {
                in.syncCoveragePct = camSync.coveragePct;
                in.syncMeanDeltaMs = camSync.meanDeltaMs;
                in.syncMaxDeltaMs  = camSync.maxDeltaMs;
                break;
            }
        }
    };

    // Driven by the recording snapshot, NOT by a live camera_stats() read and
    // NOT indexed by config position: the live counters are already zeroed by
    // the preview restart that runs ahead of this handler, and camera_stats()
    // indexes the compacted unit list, so a config index would silently
    // attribute one camera's numbers to another as soon as any earlier camera
    // fails to open. See VideoManager::last_recording_snapshot().
    QVector<CameraHealthInput> inputs;
    QSet<int> seen;
    const int64_t ticks = d->videoMgr->last_recording_action_ticks();
    for (const auto& snap : d->videoMgr->last_recording_snapshot()) {
        CameraHealthInput in;
        in.index            = snap.configIndex;
        in.name             = camera_label(snap.configIndex);
        in.participated     = true;
        in.framesGrabbed    = snap.framesGrabbed;
        in.framesEncoded    = snap.framesEncoded;
        in.framesDropped    = snap.framesDropped;
        in.incompleteFrames = snap.incompleteFrames;
        in.configuredFps    = snap.configuredFps;
        in.achievableFps    = snap.achievableFps;

        // One shared, group-wide count — only meaningful for a camera that was
        // actually in the Action1 target group, since a session can mix
        // Action1-armed and free-running cameras.
        if (ticks >= 0 && snap.actionCommandReady) {
            in.actionTicksFired = ticks;
        }

        fill_sync(in, snap.configIndex);
        seen.insert(snap.configIndex);
        inputs.push_back(in);
    }

    // Configured but never opened (duplicate serial, failed open(), dead
    // link). Reported explicitly rather than omitted — a silently missing
    // camera is exactly the failure most worth noticing.
    //
    // Skipped entirely for an audio-only session: with video deliberately
    // disabled, every configured camera would otherwise be reported as
    // "not opened" and graded Poor, turning a perfectly successful
    // audio-only recording into a wall of red.
    //
    // Only the cameras this session was meant to record: in interview mode the
    // other cameras are closed on purpose, and listing them as failures would
    // grade every interview Poor.
    if (d->settings.record.enableVideo) {
        for (const int i : d->settings.video.recorded_camera_indices()) {
            if (seen.contains(i)) {
                continue;
            }
            CameraHealthInput in;
            in.index        = i;
            in.name         = camera_label(i);
            in.participated = false;
            inputs.push_back(in);
        }
    }

    std::sort(
        inputs.begin(), inputs.end(),
        [](const CameraHealthInput& a, const CameraHealthInput& b) { return a.index < b.index; });

    const QString name = QFileInfo(sessionPath).fileName();
    const auto report  = build_session_health_report(sessionPath, name, durationMs, inputs);

    auto* dlg = new SessionHealthDialog(report, this);
    dlg->show();
}

// ── Pre-flight check ───────────────────────────────────────────────────────

PreflightReport MainWindow::run_preflight() const {
    const auto& video  = d->settings.video;
    const auto& record = d->settings.record;
    const auto& audio  = d->settings.audio;

    PreflightInput in;
    in.videoEnabled = record.enableVideo;
    in.audioEnabled = record.enableAudio;
    in.directory    = record.directory;

    // Cameras: the ones this session will record — every configured camera,
    // or only the interview camera in interview mode, whose other cameras are
    // closed on purpose and must not read as "not open". Matched against what
    // is open by configured index — camera_stats() and
    // camera_action_command_ready() take a position in the *opened* list,
    // which differs from the configured index as soon as any camera failed to
    // open (same reconciliation as show_session_health()).
    int openedCount = 0;
    if (in.videoEnabled) {
        QHash<int, int> positionOf;
        if (d->videoMgr) {
            for (int p = 0; p < d->videoMgr->camera_count(); ++p) {
                positionOf.insert(d->videoMgr->camera_config_index(p), p);
            }
        }
        for (const int i : video.recorded_camera_indices()) {
            // What the camera runs with: in interview mode its own crop and
            // rate, free-running — judging it by the room settings would
            // report "not synchronised" for a camera that is meant not to be.
            const CameraParameters cfg =
                video.interview_active()
                    ? interview_camera_params(video.cameras[static_cast<size_t>(i)],
                                              video.interview)
                    : video.cameras[static_cast<size_t>(i)];
            PreflightCamera cam;
            cam.configIndex   = i;
            cam.configuredFps = cfg.fps;
            cam.fixedRate     = cfg.specifyFps;
            cam.wantsAction1  = cfg.hwTriggerEnabled && cfg.hwTriggerSource == "Action1";
            const auto it     = positionOf.constFind(i);
            if (it != positionOf.constEnd()) {
                const auto stats   = d->videoMgr->camera_stats(*it);
                cam.opened         = true;
                cam.grabberRunning = stats.grabberRunning;
                // Frame timestamps are on the elapsed_ns() clock, so "now" is too.
                if (stats.lastFrameElapsedNs >= 0) {
                    cam.lastFrameAgeSec =
                        static_cast<double>(elapsed_ns() - stats.lastFrameElapsedNs) / 1e9;
                }
                cam.achievableFps = stats.achievableFps;
                cam.configuredFps = stats.configuredFps > 0.0 ? stats.configuredFps : cfg.fps;
                cam.action1Ready  = d->videoMgr->camera_action_command_ready(*it);
                ++openedCount;
            }
            in.cameras.push_back(cam);
        }
    }

    // Microphones: a configured device that is not present would be replaced
    // by the default input without a word (AudioRecorder's find_device()).
    // An empty device id *means* the default input, so it is never missing.
    if (in.audioEnabled) {
        in.configuredMics = static_cast<int>(audio.microphones.size());
        QSet<QByteArray> present;
        for (const auto& dev : AudioManager::available_inputs()) {
            present.insert(dev.id());
        }
        for (const auto& mic : audio.microphones) {
            if (!mic.deviceId.isEmpty() && !present.contains(mic.deviceId.toLatin1())) {
                in.missingMics << (mic.friendlyName.isEmpty() ? mic.deviceId : mic.friendlyName);
            }
        }
    }

    // Disk: what the cameras that will actually record write, plus audio.
    // libx264 encodes at constant quality, so its rate is not a setting — the
    // bitrate field stands in for it and the figure is worded as rough.
    in.freeBytes   = free_bytes_for(record.directory);
    in.rateIsRough = in.videoEnabled && video.codec == "libx264";
    in.bytesPerSec = estimate_recording_bytes_per_sec(in.videoEnabled ? openedCount : 0,
                                                      video.bitrate, 0, 0, 0, audio.codec);
    if (in.audioEnabled) {
        for (const auto& mic : audio.microphones) {
            in.bytesPerSec += estimate_recording_bytes_per_sec(0, 0, 1, mic.sampleRate,
                                                               mic.channels, audio.codec);
        }
    }

    return evaluate_preflight(in);
}

// ── Camera reopen / interview mode ─────────────────────────────────────────

void MainWindow::reopen_cameras(const QString& why) {
    if (!d->videoMgr || !d->bridge) return;
    if (d->reopenInFlight) {
        d->reopenPending = true; // one more pass once this one lands
        return;
    }
    if ((d->recordMgr && d->recordMgr->is_recording()) || d->bridge->startPending()) {
        d->reopenPending = true;
        log_warning(QString("[Main] %1 — the cameras are not reopened during a recording "
                            "(that would end its video). They will reopen when it stops.")
                        .arg(why));
        return;
    }
    d->reopenPending  = false;
    d->reopenInFlight = true;
    d->bridge->set_interview_switching(true);

    // Sequence:
    //   1. set_camera_count(0) — QML immediately hides every camera delegate.
    //      This must happen BEFORE close() so the render thread never tries to
    //      sync a scenegraph that is removing a live, actively-rendering slot.
    //   2. close() — stops grabbers, closes Pylon devices, drains queued events.
    //   3. singleShot(0) — gives the event loop one pass so QML can fully process
    //      the count-0 change (delegates torn down cleanly) before open() blocks.
    //   4. open() + set_camera_count(configured) — bring up the new camera set
    //      and restore the slot count only after hardware is ready.
    log_info(QString("[Main] reopen (%1): hiding display (%2 → 0 cameras)")
                 .arg(why)
                 .arg(d->settings.video.cameras.size()));
    d->bridge->set_camera_count(0);
    d->videoMgr->close();
    QTimer::singleShot(0, this, [this, why] {
        const int opened = d->videoMgr->open(d->settings.video);
        log_info(QString("[Main] reopen (%1): %2 camera(s) opened").arg(why).arg(opened));
        // Size the monitor for every *configured* slot, not just the
        // ones that opened successfully. Each VideoGrabber keeps its
        // original config-array position as its cameraIndex (used for
        // video_N.mp4/timestamps_camN.csv naming) even when an earlier
        // camera in the list fails to open — so if the monitor were
        // sized to the opened count instead, a later camera's real
        // cameraIndex could fall outside the tile range the QML
        // Repeater creates, silently hiding its feed behind an
        // unrelated tile while the failed camera's rightful tile sits
        // on an uninitialized placeholder. Interview mode relies on the
        // same: its one tile is addressed by its configured index.
        d->bridge->set_camera_count(static_cast<int>(d->settings.video.cameras.size()));
        if (opened > 0) {
            d->videoMgr->start_preview();
        }
        d->bridge->refresh_video_settings();
        publish_interview_rates();
        d->bridge->set_interview_switching(false);
        if (d->videoSettingsW) {
            d->videoSettingsW->sync_interview_from_settings();
        }
        d->reopenInFlight = false;
        if (d->reopenPending) {
            QTimer::singleShot(0, this, [this] { reopen_cameras("coalesced request"); });
        }
    });
}

void MainWindow::publish_interview_rates() {
    if (!d->videoMgr || !d->bridge) return;
    const int idx = d->videoMgr->interview_camera_index();
    // What the camera says it can do with the crop just applied, at once —
    // the measured rate follows a few seconds later via achievable_fps_changed.
    const double cameraMax = idx >= 0 ? d->videoMgr->camera_max_fps(idx) : -1.0;
    d->bridge->set_interview_rates(cameraMax, -1.0);
    if (d->videoSettingsW) {
        d->videoSettingsW->set_interview_camera_max_fps(cameraMax);
    }
}

void MainWindow::set_interview_mode(bool on) {
    auto& interview   = d->settings.video.interview;
    const auto resync = [this] {
        if (d->bridge) d->bridge->refresh_video_settings();
        if (d->videoSettingsW) d->videoSettingsW->sync_interview_from_settings();
    };

    QString refusal;
    if ((d->recordMgr && d->recordMgr->is_recording()) ||
        (d->bridge && d->bridge->startPending())) {
        refusal = "a recording is running or about to start";
    } else if (d->reopenInFlight) {
        refusal = "the cameras are still reopening from the previous change";
    } else if (on &&
               (interview.cameraIndex < 0 ||
                interview.cameraIndex >= static_cast<int>(d->settings.video.cameras.size()))) {
        refusal = QString("%1 is not a configured camera")
                      .arg(mosaic::camera_label(interview.cameraIndex));
    }
    if (!refusal.isEmpty()) {
        log_warning(QString("[Main] Interview mode not switched %1: %2.")
                        .arg(on ? "on" : "off")
                        .arg(refusal));
        resync();
        return;
    }
    if (interview.enabled == on && d->settings.video.interview_active() == on) {
        resync();
        return;
    }

    interview.enabled = on;
    log_info(on ? QString("[Main] Interview mode on — recording %1 only, %2\xd7%3 @ %4 fps.")
                      .arg(mosaic::camera_label(interview.cameraIndex))
                      .arg(interview.width)
                      .arg(interview.height)
                      .arg(interview.fps)
                : QString("[Main] Interview mode off — recording every camera."));
    resync();
    reopen_cameras(on ? "interview mode on" : "interview mode off");
}

// ── Close ──────────────────────────────────────────────────────────────────

void MainWindow::closeEvent(QCloseEvent* event) {
    if (d->recordMgr && d->recordMgr->is_recording()) {
        d->recordMgr->stop();
    }
    QSettings prefs("CSRU", "MOSAIC");
    prefs.setValue("mainWindow/geometry", saveGeometry());
    prefs.setValue("mainWindow/state", saveState());
    QMainWindow::closeEvent(event);
}

} // namespace mosaic
