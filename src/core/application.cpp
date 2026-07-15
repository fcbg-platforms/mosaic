#include "core/application.hpp"
#include "analysis/analysis_manager.hpp"
#include "auth/profile_manager.hpp"
#include "trigger/trigger_types.hpp"
#include "ui/main_window.hpp"
#include "utils/logger.hpp"
#include "utils/timestamp.hpp"
#include <QCoreApplication>
#include <QDir>

namespace mosaic {

struct Application::Impl {
    QString                          username;
    AppSettings                      settings;
    std::unique_ptr<TriggerManager>  triggerManager;
    std::unique_ptr<AudioManager>    audioManager;
    std::unique_ptr<VideoManager>    videoManager;
    std::unique_ptr<RecordManager>   recordManager;
    std::unique_ptr<AnalysisManager> analysisManager;
    std::unique_ptr<MainWindow>      mainWindow;
};

Application::Application(QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>()) {}

Application::~Application() = default;

void Application::initialize(const QString& username) {
    d->username = username.isEmpty() ? "guest" : username;

    // Open log file in the profile directory (or default location for guest).
    const QString settingsPath = (d->username == "guest")
        ? AppSettings::default_path()
        : ProfileManager::settings_path(d->username);

    const QString logPath = (d->username == "guest")
        ? QFileInfo(settingsPath).dir().absoluteFilePath("mosaic.log")
        : ProfileManager::log_path(d->username);

    QDir().mkpath(QFileInfo(logPath).absolutePath());
    Logger::instance().open_log_file(logPath);

    log_info(QString("MOSAIC v%1 — user: @%2 — elapsed: %3 ms")
                 .arg(QCoreApplication::applicationVersion())
                 .arg(d->username)
                 .arg(elapsed_ms()));

    if (auto loaded = AppSettings::load(settingsPath)) {
        d->settings = std::move(*loaded);
    }

    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, &Application::shutdown);

    d->triggerManager = std::make_unique<TriggerManager>(d->settings.trigger, this);
    d->audioManager   = std::make_unique<AudioManager>(this);
    d->videoManager   = std::make_unique<VideoManager>(d->triggerManager.get(), this);

    // camera_error relays both grab failures (VideoGrabber::grab_error) and
    // encode failures (VideoEncoder::encoding_error) — without this connection
    // those errors are emitted but never surface anywhere, so a recording can
    // silently produce zero output with no visible cause.
    connect(d->videoManager.get(), &VideoManager::camera_error,
            this, [](int cameraIndex, const QString& message) {
        log_error(QString("[Camera %1] %2").arg(cameraIndex).arg(message));
    });

    if (!d->settings.video.cameras.empty()) {
        d->videoManager->open(d->settings.video);
        d->videoManager->start_preview();
    }

    d->recordManager = std::make_unique<RecordManager>(
        d->settings, d->triggerManager.get(),
        d->audioManager.get(), d->videoManager.get(), d->username, this);

    // Trigger → action binding: let triggers start/stop recording automatically.
    connect(d->triggerManager.get(), &TriggerManager::action_requested,
            this, [this](TriggerAction action, const TriggerEvent& /*event*/) {
        if (action == TriggerAction::StartRecording) {
            if (!d->recordManager->is_recording()) {
                [[maybe_unused]] const bool started = d->recordManager->start();
            }
        } else if (action == TriggerAction::StopRecording) {
            if (d->recordManager->is_recording())  { d->recordManager->stop(); }
        }
    });

    // Start audio monitoring immediately so the waveform widget shows live levels.
    if (!d->settings.audio.microphones.empty()) {
        d->audioManager->start_monitoring(d->settings.audio.microphones);
    }

    // Restart monitoring and preview after each recording ends.
    connect(d->recordManager.get(), &RecordManager::recording_stopped,
            this, [this](const QString& /*path*/, int /*durationMs*/) {
        if (!d->settings.audio.microphones.empty()) {
            d->audioManager->start_monitoring(d->settings.audio.microphones);
        }
        if (!d->settings.video.cameras.empty()) {
            d->videoManager->start_preview();
        }
    });

    // Analysis manager — post-recording pose estimation
    d->analysisManager = std::make_unique<AnalysisManager>(this);
    connect(d->analysisManager.get(), &AnalysisManager::output_received,
            this, [](const QString& line) { log_info("[Analysis] " + line); });
    connect(d->analysisManager.get(), &AnalysisManager::setup_error,
            this, [](const QString& msg) { log_error("[Analysis] " + msg); });
    connect(d->recordManager.get(), &RecordManager::recording_stopped,
            this, [this](const QString& path, int /*durationMs*/) {
        d->analysisManager->analyze_session(path);
    });

    d->mainWindow = std::make_unique<MainWindow>(
        d->settings, d->username,
        d->triggerManager.get(), d->audioManager.get(),
        d->videoManager.get(), d->recordManager.get(),
        d->analysisManager.get());
    d->mainWindow->show();

    log_info("Initialisation complete.");
    emit initialized();
}

void Application::shutdown() {
    log_info("Shutting down…");

    const QString settingsPath = (d->username == "guest")
        ? AppSettings::default_path()
        : ProfileManager::settings_path(d->username);

    if (!d->settings.save(settingsPath)) {
        log_error("Failed to save settings on shutdown.");
    }

    if (d->mainWindow) { d->mainWindow->close(); }
    Logger::instance().close_log_file();
    emit shutdown_complete();
}

AppSettings&       Application::settings()       { return d->settings; }
const AppSettings& Application::settings() const { return d->settings; }
QString            Application::active_username()  const { return d->username; }
TriggerManager*    Application::trigger_manager()  const { return d->triggerManager.get(); }
AudioManager*      Application::audio_manager()    const { return d->audioManager.get();   }
VideoManager*      Application::video_manager()    const { return d->videoManager.get();   }
RecordManager*     Application::record_manager()   const { return d->recordManager.get();  }

} // namespace mosaic
