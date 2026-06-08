#include "ui/main_window.hpp"
#include "ui/audio/audio_settings_w.hpp"
#include "ui/calibration/calibration_w.hpp"
#include "ui/logger/logger_panel_w.hpp"
#include "ui/monitor_bridge.hpp"
#include "ui/record/record_settings_w.hpp"
#include "ui/session/session_browser_w.hpp"
#include "ui/trigger/trigger_event_panel_w.hpp"
#include "ui/trigger/trigger_settings_w.hpp"
#include "ui/video/performance_monitor_w.hpp"
#include "ui/video/video_settings_w.hpp"
#include "utils/logger.hpp"
#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QQmlContext>
#include <QQuickWidget>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

namespace mosaic {

struct MainWindow::Impl {
    AppSettings&     settings;
    QString          username;
    TriggerManager*  triggerMgr   = nullptr;
    AudioManager*    audioMgr     = nullptr;
    VideoManager*    videoMgr     = nullptr;
    RecordManager*   recordMgr    = nullptr;
    AnalysisManager* analysisMgr  = nullptr;
    MonitorBridge*   bridge       = nullptr;

    QSplitter*      mainSplitter  = nullptr;
    QSplitter*      rightSplitter = nullptr;
    QTabWidget*     settingsTabs  = nullptr;
    QQuickWidget*   monitorView   = nullptr;
    LoggerPanelW*   loggerPanel   = nullptr;
    QLabel*         statusLabel   = nullptr;

    explicit Impl(AppSettings& s, const QString& user, TriggerManager* tm,
                  AudioManager* am, VideoManager* vm, RecordManager* rm,
                  AnalysisManager* anlm)
        : settings(s), username(user)
        , triggerMgr(tm), audioMgr(am), videoMgr(vm), recordMgr(rm)
        , analysisMgr(anlm) {}
};

MainWindow::MainWindow(AppSettings&     settings,
                        const QString&   username,
                        TriggerManager*  triggerMgr,
                        AudioManager*    audioMgr,
                        VideoManager*    videoMgr,
                        RecordManager*   recordMgr,
                        AnalysisManager* analysisMgr,
                        QWidget*         parent)
    : QMainWindow(parent)
    , d(std::make_unique<Impl>(settings, username, triggerMgr, audioMgr, videoMgr, recordMgr, analysisMgr))
{
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

// ── Menu bar ───────────────────────────────────────────────────────────────

void MainWindow::build_menu_bar() {
    auto* file = menuBar()->addMenu("&File");

    auto* openAction = new QAction("&Open recordings folder…", this);
    openAction->setShortcut(QKeySequence("Ctrl+Shift+O"));
    connect(openAction, &QAction::triggered, this, [this] {
        const QString path = d->recordMgr
            ? d->recordMgr->current_session_path()
            : d->settings.record.directory;
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    file->addAction(openAction);

    auto* browseAction = new QAction("&Browse Sessions…", this);
    browseAction->setShortcut(QKeySequence("Ctrl+B"));
    connect(browseAction, &QAction::triggered, this, [this] {
        SessionBrowserW browser(d->settings.record.directory, d->analysisMgr, this);
        browser.exec();
    });
    file->addAction(browseAction);
    file->addSeparator();

    auto* switchAction = new QAction(
        QString("Switch profile  (current: %1)").arg(
            d->username == "guest" ? "Guest" : "@" + d->username),
        this);
    switchAction->setShortcut(QKeySequence("Ctrl+Shift+P"));
    connect(switchAction, &QAction::triggered, this, [this] {
        if (d->recordMgr && d->recordMgr->is_recording()) {
            d->recordMgr->stop();
        }
        emit switch_profile_requested();
        // Exit code 42 signals main() to re-show the login dialog
        // rather than quit entirely.
        QCoreApplication::exit(42);
    });
    file->addAction(switchAction);
    file->addSeparator();

    auto* quitAction = new QAction("&Quit", this);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered,
            QCoreApplication::instance(), &QCoreApplication::quit);
    file->addAction(quitAction);

    auto* view = menuBar()->addMenu("&View");
    view->addAction("Reset layout", this, [this] {
        d->mainSplitter->setSizes({360, width() - 360});
        d->rightSplitter->setSizes({height() - 200, 200});
    });
    view->addSeparator();

    auto* logToggle = new QAction("Show &Log Panel", this);
    logToggle->setShortcut(QKeySequence("Ctrl+L"));
    logToggle->setCheckable(true);
    logToggle->setChecked(true);
    connect(logToggle, &QAction::toggled, this, [this](bool shown) {
        if (d->loggerPanel) { d->loggerPanel->setVisible(shown); }
    });
    view->addAction(logToggle);

    auto* record = menuBar()->addMenu("&Record");
    auto* startAction = new QAction("▶  Start recording", this);
    startAction->setShortcut(QKeySequence("Ctrl+R"));
    connect(startAction, &QAction::triggered, this, [this] {
        if (d->bridge) { d->bridge->startRecording(); }
    });
    record->addAction(startAction);

    auto* stopAction = new QAction("■  Stop recording", this);
    stopAction->setShortcut(QKeySequence("Ctrl+."));
    connect(stopAction, &QAction::triggered, this, [this] {
        if (d->bridge) { d->bridge->stopRecording(); }
    });
    record->addAction(stopAction);

    auto* help = menuBar()->addMenu("&Help");
    help->addAction("&About MOSAIC…", this, [this] {
        QMessageBox::about(this, "About MOSAIC",
            "<b>MOSAIC</b> v" + QCoreApplication::applicationVersion() +
            "<br>Multi-camera Observatory for Social &amp; Activity Interaction Capture"
            "<br><br>CSRU Lab");
    });
}

// ── Central widget ─────────────────────────────────────────────────────────

void MainWindow::build_central_widget() {
    // Outer horizontal split: settings sidebar | right pane
    d->mainSplitter = new QSplitter(Qt::Horizontal, this);
    d->mainSplitter->setHandleWidth(2);
    setCentralWidget(d->mainSplitter);

    // ── Left: settings tabs ───────────────────────────────────────────────
    d->settingsTabs = new QTabWidget;
    d->settingsTabs->setMinimumWidth(300);
    d->settingsTabs->setMaximumWidth(520);
    d->settingsTabs->setDocumentMode(true);

    d->settingsTabs->addTab(
        new VideoSettingsW(d->settings.video),                              "Video");
    d->settingsTabs->addTab(
        new AudioSettingsW(d->settings.audio, d->audioMgr),                "Audio");
    d->settingsTabs->addTab(
        new TriggerSettingsW(d->settings.trigger, d->triggerMgr),      "Triggers");
    d->settingsTabs->addTab(
        new TriggerEventPanelW(d->triggerMgr),                            "Events");
    d->settingsTabs->addTab(
        new RecordSettingsW(d->settings.record),                           "Record");
    d->settingsTabs->addTab(
        new PerformanceMonitorW(d->videoMgr, d->audioMgr, d->analysisMgr),  "Perf");
    d->settingsTabs->addTab(
        new CalibrationW(d->settings.video),                           "Calibrate");

    d->mainSplitter->addWidget(d->settingsTabs);

    // ── Right: vertical split — monitor view on top, log panel on bottom ──
    d->rightSplitter = new QSplitter(Qt::Vertical);
    d->rightSplitter->setHandleWidth(3);

    // QML monitoring view
    d->monitorView = new QQuickWidget;
    d->monitorView->setResizeMode(QQuickWidget::SizeRootObjectToView);
    d->monitorView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    d->bridge = new MonitorBridge(d->recordMgr, d->settings.video, this);
    d->monitorView->rootContext()->setContextProperty("backend", d->bridge);
    d->monitorView->setSource(QUrl("qrc:/qml/MonitorView.qml"));

    if (d->monitorView->status() == QQuickWidget::Error) {
        for (const auto& err : d->monitorView->errors()) {
            log_error(QString("QML error: %1").arg(err.toString()));
        }
    }

    d->rightSplitter->addWidget(d->monitorView);

    // Logger panel
    d->loggerPanel = new LoggerPanelW;
    d->loggerPanel->setMaximumHeight(280);
    d->loggerPanel->setMinimumHeight(80);
    d->rightSplitter->addWidget(d->loggerPanel);

    d->rightSplitter->setStretchFactor(0, 1);
    d->rightSplitter->setStretchFactor(1, 0);
    d->rightSplitter->setSizes({700, 180});

    d->mainSplitter->addWidget(d->rightSplitter);
    d->mainSplitter->setStretchFactor(0, 0);
    d->mainSplitter->setStretchFactor(1, 1);
    d->mainSplitter->setSizes({380, 1300});
}

// ── Status bar ─────────────────────────────────────────────────────────────

void MainWindow::build_status_bar() {
    d->statusLabel = new QLabel("Ready");
    statusBar()->addWidget(d->statusLabel);

    // User chip in the right corner of the status bar.
    const QString userLabel = (d->username == "guest")
        ? "👤  Guest session"
        : QString("👤  @%1").arg(d->username);
    auto* userChip = new QLabel(userLabel);
    userChip->setStyleSheet(
        "QLabel { background: #12122a; border: 1px solid #252545; border-radius: 10px;"
        "  padding: 2px 10px; color: #7878a0; font-size: 11px; }");
    userChip->setToolTip("Current profile — use File → Switch profile to change");
    statusBar()->addPermanentWidget(userChip);

    // Show the last warning/error in the status bar as a quick heads-up.
    connect(&Logger::instance(), &Logger::entry_added,
            this, [this](int level, QString /*ts*/, QString /*loc*/, QString msg) {
        if (level >= static_cast<int>(LogLevel::Warning)) {
            d->statusLabel->setText(msg);
        }
    }, Qt::QueuedConnection);

    if (d->recordMgr) {
        connect(d->recordMgr, &RecordManager::recording_started, this,
                [this](const QString& path) {
            d->statusLabel->setText(QString("● Recording  →  %1").arg(path));
            d->statusLabel->setStyleSheet("color: #ff6666; font-weight: bold;");
        });
        connect(d->recordMgr, &RecordManager::recording_stopped, this,
                [this](const QString& /*path*/, int durationMs) {
            d->statusLabel->setText(
                QString("Recording stopped. Duration: %1 s")
                    .arg(durationMs / 1000.0, 0, 'f', 1));
            d->statusLabel->setStyleSheet("");
        });
    }

    // Show frame-drop warnings from VideoManager.
    if (d->videoMgr) {
        connect(d->videoMgr, &VideoManager::frame_dropped, this,
                [this](int cameraIndex, int64_t frameId) {
            d->statusLabel->setText(
                QString("⚠ Frame dropped — camera %1, frame %2")
                    .arg(cameraIndex).arg(frameId));
            d->statusLabel->setStyleSheet("color: #ddaa44;");
        }, Qt::QueuedConnection);
    }
}

// ── Close ──────────────────────────────────────────────────────────────────

void MainWindow::closeEvent(QCloseEvent* event) {
    if (d->recordMgr && d->recordMgr->is_recording()) {
        d->recordMgr->stop();
    }
    QSettings prefs("CSRU", "MOSAIC");
    prefs.setValue("mainWindow/geometry", saveGeometry());
    prefs.setValue("mainWindow/state",    saveState());
    QMainWindow::closeEvent(event);
}

} // namespace mosaic
