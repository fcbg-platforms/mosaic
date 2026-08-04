#include "core/application.hpp"
#include "analysis/analysis_manager.hpp"
#include "auth/profile_manager.hpp"
#include "trigger/trigger_types.hpp"
#include "ui/main_window.hpp"
#include "utils/logger.hpp"
#include "utils/timestamp.hpp"
#include <QCoreApplication>
#include <QDir>
#include <array>

namespace {

// Pre-configured camera list for a brand-new profile that has no
// settings.json yet (register_profile() only creates the profile
// directory — it never writes a settings file, see profile_manager.cpp).
// Matches this app's real room-11 rig: 6 Basler acA1920-25gc units, one
// per isolated GigE NIC, sharing one GigE Action Command trigger group.
// Only serialNumber/friendlyName are set per entry — every other field
// uses CameraParameters' own defaults, which already default to Action1
// hardware-trigger sync (see CameraParameters::hwTriggerEnabled/
// hwTriggerSource in settings.hpp), so a seeded slot behaves identically
// to one added by hand through the UI today. Order matches the room-
// position mapping already used in this rig's settings.json.
std::vector<mosaic::CameraParameters> default_room11_cameras() {
    static constexpr std::array<const char*, 6> kSerials = {
        "24925616", "24925618", "24925620", "24925615", "24925621", "24893039",
    };
    std::vector<mosaic::CameraParameters> cameras;
    cameras.reserve(kSerials.size());
    for (size_t i = 0; i < kSerials.size(); ++i) {
        mosaic::CameraParameters cam;
        cam.serialNumber = QString::fromLatin1(kSerials[i]);
        cam.friendlyName = QString("Camera %1").arg(i + 1);
        cameras.push_back(cam);
    }
    return cameras;
}

// Pre-configured microphone list for a brand-new profile — mirrors
// default_room11_cameras() above, but simpler: unlike cameras, there's no
// stable "serial number" to seed here, since the goal is just "record from
// whatever the OS considers the default input," not one specific room-11
// unit. A single default-constructed entry (empty deviceId) is exactly
// that — AudioRecorder::find_device() falls through to
// QMediaDevices::defaultAudioInput() with no warning whenever deviceId is
// empty.
std::vector<mosaic::MicrophoneParameters> default_microphone() {
    return { mosaic::MicrophoneParameters{} };
}

} // namespace

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

    // AppSettings::load() returns a valid (default-constructed) AppSettings
    // even when settingsPath doesn't exist yet — it only returns
    // std::nullopt on an actual JSON parse error — so "no settings file"
    // must be checked explicitly here, not inferred from the optional being
    // engaged.
    const bool settingsFileExisted = QFileInfo::exists(settingsPath);
    if (auto loaded = AppSettings::load(settingsPath)) {
        d->settings = std::move(*loaded);
    }
    if (!settingsFileExisted) {
        // No settings.json yet — a brand-new profile. Seed it with the
        // real room-11 camera rig instead of starting with zero cameras
        // configured, so a newly registered profile is immediately usable
        // without a manual "add camera" pass for each of the 6 units.
        d->settings.video.cameras = default_room11_cameras();
        // Seed one default microphone too, using the system's default
        // audio input device — same reasoning as the camera seed: a new
        // profile should be able to record audio immediately, not stay
        // silent (and never even create a session's audio/ folder, since
        // RecordManager::start() skips the whole audio block when the
        // microphone list is empty) until a mic is added by hand.
        d->settings.audio.microphones = default_microphone();
    }

    // Guarantee reference stability before videoManager->open() below binds
    // a live VideoGrabber::Impl::params reference into each element of this
    // vector (for that grabber's entire lifetime). Required regardless of
    // which path populated `cameras` above: VideoSettings::from_json()
    // already reserves this same cap internally, but that reservation does
    // not survive default_room11_cameras()'s own reserve(6)-then-move-assign
    // path for a brand-new profile — so this call is the one place that
    // covers both. See VideoSettings::kMaxCameras's doc comment (settings.hpp)
    // for the full explanation of why a reallocation here would be a
    // use-after-free (this was a real, confirmed crash: any camera-settings
    // edit after this point would dereference freed memory).
    d->settings.video.cameras.reserve(VideoSettings::kMaxCameras);
    // Mirrors the video reservation immediately above: AudioManager::
    // start_monitoring() (called below, before AudioSettingsW/
    // MicrophoneCardW exist) binds a live `const MicrophoneParameters&`
    // into each AudioRecorder for that recorder's entire lifetime (see
    // AudioRecorder::m_params) — a reallocation of this vector after that
    // point is a use-after-free the next time a recorder reads m_params.
    d->settings.audio.microphones.reserve(AudioSettings::kMaxMicrophones);

    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, &Application::shutdown);

    d->triggerManager = std::make_unique<TriggerManager>(d->settings.trigger, this);
    d->audioManager   = std::make_unique<AudioManager>(this);
    d->videoManager   = std::make_unique<VideoManager>(this);

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
        if (d->analysisManager->auto_analyze()) {
            d->analysisManager->analyze_session(path);
        }
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
