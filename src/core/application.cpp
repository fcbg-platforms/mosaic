#include "core/application.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QSet>
#include <array>

#include "analysis/analysis_manager.hpp"
#include "auth/profile_manager.hpp"
#include "core/recording_access_control.hpp"
#include "session/session_info.hpp"
#include "trigger/trigger_types.hpp"
#include "ui/main_window.hpp"
#include "utils/logger.hpp"
#include "utils/timestamp.hpp"

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
    return {mosaic::MicrophoneParameters{}};
}

// Three example keyboard triggers for a brand-new profile, each already bound
// to a key and carrying a distinct numeric code. Names/keys/codes are a
// starting point to edit, not a prescription — the point is that the
// multi-trigger capability is visible at all (TriggerSettings::keyboardTriggers
// is otherwise an empty vector, so a fresh profile shows nothing).
std::vector<mosaic::KeyTriggerConfig> default_keyboard_triggers() {
    std::vector<mosaic::KeyTriggerConfig> t;
    const struct {
        const char* name;
        const char* key;
        int code;
    } seeds[] = {
        {"Experiment start", "F9", 1},
        {"Trial onset", "F11", 2},
        {"Note", "F12", 3},
    };
    for (const auto& s : seeds) {
        mosaic::KeyTriggerConfig c;
        c.name   = s.name;
        c.keySeq = s.key;
        c.code   = s.code;
        t.push_back(std::move(c));
    }
    return t;
}

// Resolves every OTHER known profile's own configured record.directory
// (reading each one's real settings.json, not assuming the default
// convention holds if that profile customized it) — reused verbatim
// infrastructure (ProfileManager::settings_path() + AppSettings::load()),
// not a new aggregation mechanism. Excludes `excludeUsername` (the calling
// admin's own profile) since SessionBrowserW/AnalysisTabW already scan
// their own settings.record.directory separately — including it here too
// would show every one of the admin's own sessions twice. Always includes
// the shared "_unassigned" fallback so migrated-but-unrecognized sessions
// stay visible to every admin.
QStringList resolve_other_user_directories(const mosaic::ProfileManager& profileMgr,
                                           const QString& excludeUsername) {
    QStringList dirs;
    for (const auto& profile : profileMgr.profiles()) {
        if (profile.username == excludeUsername) {
            continue;
        }
        const QString settingsPath = mosaic::ProfileManager::settings_path(profile.username);
        if (auto loaded = mosaic::AppSettings::load(settingsPath)) {
            dirs << loaded->record.directory;
        } else {
            // Profile registered but never actually logged in yet (no
            // settings.json written) — its future recordings will still
            // land at the default convention once it does, so include that
            // now rather than waiting for its first login to appear.
            dirs << mosaic::default_record_directory_for(profile.username);
        }
    }
    // "guest" is a special-cased pseudo-identity (see Application::initialize(),
    // which loads its settings from AppSettings::default_path() rather than
    // ProfileManager::settings_path()) — it is never a registered Profile, so
    // the loop above never finds it. Without this, an admin's aggregate view
    // would silently miss guest's own recordings even though guest's own
    // record.directory is seeded/migrated to the exact same per-user
    // convention as every real profile (see the settingsFileExisted block
    // below). Guest itself can never be admin (main.cpp's is_admin() lookup
    // finds no registered "guest" Profile and defaults to false), so this
    // branch is unreachable in practice today — kept for defensive
    // correctness in case that assumption ever changes.
    if (excludeUsername != "guest") {
        if (auto loaded = mosaic::AppSettings::load(mosaic::AppSettings::default_path())) {
            dirs << loaded->record.directory;
        } else {
            dirs << mosaic::default_record_directory_for("guest");
        }
    }
    dirs << mosaic::unassigned_record_directory();
    return dirs;
}

// One-time, naturally-idempotent migration of sessions still sitting loose
// in the pre-item-27 shared flat folder (legacy_shared_record_directory())
// into the new per-user layout — sorted by each session's own recorded_by
// field via the exact same "does this child dir contain session_meta.json
// directly" detection SessionInfo::list_all() already uses. Naturally
// idempotent because nothing is left loose in the flat root for a later
// call to find once a session has been moved — no separate marker file
// needed. Only ever called when the active profile is an admin (see
// initialize()) — a filesystem reorganization touching other users' data
// shouldn't be triggerable by an ordinary non-admin login.
void migrate_flat_session_folders(const mosaic::ProfileManager& profileMgr) {
    const QDir flatRoot(mosaic::legacy_shared_record_directory());
    if (!flatRoot.exists()) {
        return;
    }

    QSet<QString> knownUsernames;
    for (const auto& profile : profileMgr.profiles()) {
        knownUsernames.insert(profile.username);
    }
    // "guest" is a special-cased pseudo-identity, never a registered Profile
    // (see resolve_other_user_directories()'s doc comment above) — without
    // this, a flat-folder session with recorded_by=="guest" would be
    // misrouted to _unassigned instead of guest's own per-user folder.
    knownUsernames.insert("guest");

    const auto entries = flatRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& entry : entries) {
        const QString sourceDir = entry.filePath();
        if (!QFile::exists(sourceDir + "/session_meta.json")) {
            continue;
        } // not a session folder

        const mosaic::SessionInfo info = mosaic::SessionInfo::load(sourceDir);
        const QString targetOwnerDir =
            mosaic::resolve_migration_target(info.recordedBy, knownUsernames);

        QDir().mkpath(targetOwnerDir);
        const QString targetDir = targetOwnerDir + "/" + entry.fileName();
        if (QFile::exists(targetDir)) {
            mosaic::log_warning(QString("[Application] Migration: %1 already exists — leaving "
                                        "%2 in place, resolve the name clash manually.")
                                    .arg(targetDir, sourceDir));
            continue;
        }
        if (QDir().rename(sourceDir, targetDir)) {
            mosaic::log_info(QString("[Application] Migrated session %1 → %2 (recorded_by=\"%3\").")
                                 .arg(entry.fileName(), targetOwnerDir, info.recordedBy));
        } else {
            mosaic::log_warning(QString("[Application] Migration: failed to move %1 to %2.")
                                    .arg(sourceDir, targetDir));
        }
    }
}

// Self-scoped companion to migrate_flat_session_folders() above: moves only
// the CURRENT user's own sessions still sitting loose in the legacy flat
// folder into their own now-current per-user directory. Unlike the broad
// sweep above (every profile's sessions, gated to admin logins only, since
// it touches other users' data), this only ever moves sessions this user
// themselves recorded — safe on every login, admin or not.
//
// Runs unconditionally on every login, not just the moment record.directory
// first transitions away from the legacy default, and not just for
// newly-created profiles: a profile that already transitioned in an earlier
// session — before this function existed — would otherwise have its
// record.directory correctly repointed at its own subfolder while its
// actual pre-existing session folders stayed behind, orphaned, in the flat
// root (this was a real, confirmed regression: guest's settings.json had
// already been migrated to "./recordings/guest", but the 22 real sessions
// it had recorded were still sitting in the flat "./recordings", making
// them silently disappear from guest's own session browser/Analysis tab).
// Naturally idempotent, same reasoning as the admin sweep — nothing is left
// loose for this user once their own sessions have been moved once.
void migrate_own_flat_sessions(const QString& username, const QString& targetDir) {
    const QDir flatRoot(mosaic::legacy_shared_record_directory());
    if (!flatRoot.exists()) {
        return;
    }

    const auto entries = flatRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& entry : entries) {
        const QString sourceDir = entry.filePath();
        if (!QFile::exists(sourceDir + "/session_meta.json")) {
            continue;
        } // not a session folder

        const mosaic::SessionInfo info = mosaic::SessionInfo::load(sourceDir);
        if (info.recordedBy != username) {
            continue;
        } // not ours — leave for the admin sweep

        QDir().mkpath(targetDir);
        const QString destDir = targetDir + "/" + entry.fileName();
        if (QFile::exists(destDir)) {
            mosaic::log_warning(QString("[Application] Migration: %1 already exists — leaving "
                                        "%2 in place, resolve the name clash manually.")
                                    .arg(destDir, sourceDir));
            continue;
        }
        if (QDir().rename(sourceDir, destDir)) {
            mosaic::log_info(QString("[Application] Recovered own session %1 → %2.")
                                 .arg(entry.fileName(), targetDir));
        } else {
            mosaic::log_warning(QString("[Application] Migration: failed to move %1 to %2.")
                                    .arg(sourceDir, destDir));
        }
    }
}

} // namespace

namespace mosaic {

struct Application::Impl {
    QString username;
    bool isAdmin = false;
    // Every OTHER known profile's own record.directory (plus the shared
    // "_unassigned" fallback) — only ever populated when isAdmin is true;
    // stays empty for a regular user, whose session browsing is already
    // correctly scoped by their own (per-user) settings.record.directory
    // alone, with no aggregation needed.
    QStringList otherUserDirectories;
    AppSettings settings;
    std::unique_ptr<TriggerManager> triggerManager;
    std::unique_ptr<AudioManager> audioManager;
    std::unique_ptr<VideoManager> videoManager;
    std::unique_ptr<RecordManager> recordManager;
    std::unique_ptr<AnalysisManager> analysisManager;
    std::unique_ptr<MainWindow> mainWindow;
};

Application::Application(QObject* parent) : QObject(parent), d(std::make_unique<Impl>()) {}

Application::~Application() = default;

void Application::initialize(const QString& username, bool isAdmin) {
    d->username = username.isEmpty() ? "guest" : username;
    d->isAdmin  = isAdmin;

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
        // Seed a few example keyboard triggers. TriggerSettings::
        // keyboardTriggers otherwise defaults to an empty vector, so a new
        // profile has nothing at all and the multi-trigger capability isn't
        // discoverable — a real report ("looks like we can only give one
        // trigger"). Bound rather than left unbound on purpose: an unbound
        // trigger renders an amber "no key bound — won't fire" warning, and
        // three of those on a fresh profile reads as breakage rather than as
        // an invitation. F9/F11/F12 avoids F1 (conventionally Help) and F10
        // (activates the Windows menu bar — eventFilter deliberately doesn't
        // consume the key, so both would happen).
        d->settings.trigger.keyboardTriggers = default_keyboard_triggers();
        // Per-user recording directory (item 27) — every profile's
        // recordings live in their own subfolder from the start, not the
        // shared legacy default.
        d->settings.record.directory = default_record_directory_for(d->username);
    } else if (is_legacy_shared_record_directory(d->settings.record.directory)) {
        // Existing profile that predates per-user recording access control
        // — was still pointing at the shared legacy default, never
        // customized. One-time, idempotent fix-forward to this profile's
        // own subfolder; a profile that already customized record.directory
        // to something else is left untouched (only the literal, unmodified
        // legacy default is migrated).
        d->settings.record.directory = default_record_directory_for(d->username);
    }

    // Recover any of this user's own sessions still sitting loose in the
    // legacy flat folder (see migrate_own_flat_sessions()'s doc comment for
    // why this must run unconditionally, not just on the branches above).
    migrate_own_flat_sessions(d->username, d->settings.record.directory);

    if (d->isAdmin) {
        ProfileManager profileMgr;
        profileMgr.load();
        d->otherUserDirectories = resolve_other_user_directories(profileMgr, d->username);
        migrate_flat_session_folders(profileMgr);
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

    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
            &Application::shutdown);

    d->triggerManager = std::make_unique<TriggerManager>(d->settings.trigger, this);
    d->audioManager   = std::make_unique<AudioManager>(this);
    d->videoManager   = std::make_unique<VideoManager>(this);

    // camera_error relays both grab failures (VideoGrabber::grab_error) and
    // encode failures (VideoEncoder::encoding_error) — without this connection
    // those errors are emitted but never surface anywhere, so a recording can
    // silently produce zero output with no visible cause.
    connect(d->videoManager.get(), &VideoManager::camera_error, this,
            [](int cameraIndex, const QString& message) {
                log_error(QString("[Camera %1] %2").arg(cameraIndex).arg(message));
            });

    if (!d->settings.video.cameras.empty()) {
        d->videoManager->open(d->settings.video);
        d->videoManager->start_preview();
    }

    d->recordManager =
        std::make_unique<RecordManager>(d->settings, d->triggerManager.get(), d->audioManager.get(),
                                        d->videoManager.get(), d->username, this);

    // Trigger → action binding: let triggers start/stop recording automatically.
    connect(d->triggerManager.get(), &TriggerManager::action_requested, this,
            [this](TriggerAction action, const TriggerEvent& /*event*/) {
                if (action == TriggerAction::StartRecording) {
                    if (!d->recordManager->is_recording()) {
                        [[maybe_unused]] const bool started = d->recordManager->start();
                    }
                } else if (action == TriggerAction::StopRecording) {
                    if (d->recordManager->is_recording()) {
                        d->recordManager->stop();
                    }
                }
            });

    // Start audio monitoring immediately so the waveform widget shows live levels.
    if (!d->settings.audio.microphones.empty()) {
        d->audioManager->start_monitoring(d->settings.audio.microphones);
    }

    // Restart monitoring and preview after each recording ends.
    connect(d->recordManager.get(), &RecordManager::recording_stopped, this,
            [this](const QString& /*path*/, int /*durationMs*/) {
                if (!d->settings.audio.microphones.empty()) {
                    d->audioManager->start_monitoring(d->settings.audio.microphones);
                }
                if (!d->settings.video.cameras.empty()) {
                    d->videoManager->start_preview();
                }
            });

    // Analysis manager — post-recording pose estimation
    d->analysisManager = std::make_unique<AnalysisManager>(this);
    connect(d->analysisManager.get(), &AnalysisManager::output_received, this,
            [](const QString& line) { log_info("[Analysis] " + line); });
    connect(d->analysisManager.get(), &AnalysisManager::setup_error, this,
            [](const QString& msg) { log_error("[Analysis] " + msg); });
    connect(d->recordManager.get(), &RecordManager::recording_stopped, this,
            [this](const QString& path, int /*durationMs*/) {
                if (d->analysisManager->auto_analyze()) {
                    d->analysisManager->analyze_session(path);
                }
            });

    d->mainWindow = std::make_unique<MainWindow>(d->settings, d->username, d->triggerManager.get(),
                                                 d->audioManager.get(), d->videoManager.get(),
                                                 d->recordManager.get(), d->analysisManager.get(),
                                                 d->isAdmin, d->otherUserDirectories);
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

    if (d->mainWindow) {
        d->mainWindow->close();
    }
    Logger::instance().close_log_file();
    emit shutdown_complete();
}

AppSettings& Application::settings() { return d->settings; }
const AppSettings& Application::settings() const { return d->settings; }
QString Application::active_username() const { return d->username; }
TriggerManager* Application::trigger_manager() const { return d->triggerManager.get(); }
AudioManager* Application::audio_manager() const { return d->audioManager.get(); }
VideoManager* Application::video_manager() const { return d->videoManager.get(); }
RecordManager* Application::record_manager() const { return d->recordManager.get(); }

} // namespace mosaic
