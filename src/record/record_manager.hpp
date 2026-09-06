#pragma once
#include <QObject>
#include <memory>

#include "audio/audio_manager.hpp"
#include "core/settings.hpp"
#include "session/session_name.hpp"
#include "trigger/trigger_manager.hpp"
#include "video/video_manager.hpp"

namespace mosaic {

/// @brief Central coordinator for a recording session.
///
/// RecordManager is the single point of control for starting and stopping all
/// recording channels.  It:
///
/// - Creates a timestamped session folder under @c AppSettings::record.directory.
/// - Writes @c session_meta.json immediately on start (even if later steps fail).
/// - Starts the trigger CSV, audio recorders, and video pipeline in the correct
///   order, then stops them in reverse order.
/// - Drives a 100 ms elapsed-time ticker used by the QML monitor view.
///
/// All public methods must be called from the **main thread**.
///
/// @par Typical usage
/// @code{.cpp}
/// // Wired up by Application — most code accesses this via app.record_manager()
/// RecordManager rm(settings, triggerMgr, audioMgr, videoMgr, "my_lab");
///
/// connect(&rm, &RecordManager::recording_started, this, [](const QString& path) {
///     qDebug() << "Recording to" << path;
/// });
///
/// if (rm.start()) {
///     // recording is live
/// }
/// // ... later ...
/// rm.stop();
/// @endcode
///
/// @see AppSettings::RecordSettings, VideoManager, AudioManager, TriggerManager
class RecordManager : public QObject {
    Q_OBJECT
   public:
    /// @param settings    Application settings (record, video, audio, trigger sub-structs).
    /// @param triggerMgr  Trigger manager — receives start/stop_recording() calls.
    ///                    May be @c nullptr (triggers are then skipped).
    /// @param audioMgr    Audio manager.  May be @c nullptr.
    /// @param videoMgr    Video manager.  May be @c nullptr.
    /// @param username    Profile name embedded in @c session_meta.json as
    ///                    @c "recorded_by".  Defaults to @c "guest".
    /// @param parent      Qt parent object.
    explicit RecordManager(AppSettings& settings, TriggerManager* triggerMgr,
                           AudioManager* audioMgr, VideoManager* videoMgr = nullptr,
                           const QString& username = "guest", QObject* parent = nullptr);
    ~RecordManager() override;

    /// @brief Begins a new recording session.
    ///
    /// Creates the session folder, writes @c session_meta.json, then starts
    /// triggers → audio → video in that order.
    ///
    /// @returns @c false if already recording or the session directory cannot
    ///          be created; logs the reason on failure.
    [[nodiscard]] bool start();

    /// @brief Sets the subject/session/task/notes applied to the *next*
    ///        start().
    ///
    /// Deliberately armed state rather than an argument to start(). start()
    /// has a second caller with no UI behind it — a StartRecording trigger
    /// (Application) — and a defaulted argument there would have produced a
    /// bare timestamp folder even when the operator had just typed a subject
    /// into the monitor, silently losing attribution for exactly the sessions
    /// that are hardest to reconstruct afterwards. Armed state means a
    /// trigger-started recording inherits whatever is on screen.
    ///
    /// The run index in @p id is ignored: it is resolved against the
    /// recordings directory inside start(), so the number written is always
    /// the one that was actually free.
    ///
    /// Copied by value, so editing the fields mid-countdown cannot change a
    /// start already in flight. Ignored while recording.
    void set_session_identity(const SessionIdentity& id);

    /// @returns The identity that will be applied to the next start().
    [[nodiscard]] SessionIdentity session_identity() const;

    /// @brief Ends the current session.
    ///
    /// Stops video → audio → triggers in reverse order, then emits
    /// @c recording_stopped().  Safe to call even when not recording.
    void stop();

    /// @returns @c true while a session is active.
    [[nodiscard]] bool is_recording() const;

    /// @returns Milliseconds elapsed since the session started (0 when idle).
    [[nodiscard]] int elapsed_ms() const;

    /// @returns Absolute path to the current session folder, or an empty
    ///          string when no session has been started yet.
    [[nodiscard]] QString current_session_path() const;

   signals:
    /// Emitted when a session starts successfully.
    /// @param sessionPath  Absolute path to the session folder.
    void recording_started(QString sessionPath);

    /// Emitted when a session ends (normally or via stop()).
    /// @param sessionPath  Absolute path to the session folder.
    /// @param durationMs   Session duration in milliseconds.
    void recording_stopped(QString sessionPath, int durationMs);

    /// Emitted every 100 ms while recording is active.
    /// @param ms  Milliseconds elapsed since session start.
    void elapsed_ms_changed(int ms);

    /// Emitted when a non-recoverable error prevents recording from starting.
    /// @param message  Human-readable error description (also logged).
    void error_occurred(QString message);

   private slots:
    void tick();

   private:
    void write_session_meta() const;
    void write_session_notes() const;

    /// Creates the session folder and returns its path, or an empty string if
    /// it could not be created. Never reuses an existing directory — see the
    /// implementation for why that used to be possible.
    [[nodiscard]] QString create_session_folder() const;
    [[nodiscard]] QString build_session_path() const;
    [[nodiscard]] QString build_file_path(const QString& basename, const QString& ext) const;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
