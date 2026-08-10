#pragma once
#include "audio/audio_manager.hpp"
#include "core/settings.hpp"
#include "record/record_manager.hpp"
#include "trigger/trigger_manager.hpp"
#include "video/video_manager.hpp"
#include <QObject>
#include <QString>
#include <memory>

namespace mosaic {

/// @brief Top-level application object — owns all subsystem managers and
///        orchestrates startup and shutdown ordering.
///
/// Created once in @c main() (inside the auth loop) and destroyed when the
/// user quits or switches profiles.  All subsystem managers are owned here and
/// accessible via typed accessors.
///
/// @par Lifecycle
/// @code{.cpp}
/// Application app;
/// app.initialize("cognitive_lab");   // loads settings, opens cameras, shows window
/// QApplication::exec();             // run the event loop
/// // shutdown() is called automatically via QCoreApplication::aboutToQuit
/// @endcode
///
/// @note Not a singleton — a fresh instance is created for each profile
///       session so there is no stale state when the user switches profiles.
///
/// @see ProfileManager, AppSettings, RecordManager
class Application : public QObject {
    Q_OBJECT
public:
    explicit Application(QObject* parent = nullptr);
    ~Application() override;

    /// @brief Initialises all subsystems and shows the main window.
    ///
    /// Must be called exactly once after @c QApplication is constructed.
    /// The method:
    /// -# Opens a per-profile log file.
    /// -# Loads @c AppSettings from the profile's @c settings.json
    ///    (falls back to defaults if the file does not exist).
    /// -# Creates @c TriggerManager and installs keyboard event filters.
    /// -# Creates @c AudioManager.
    /// -# Creates @c VideoManager and opens configured cameras.
    /// -# Creates @c RecordManager.
    /// -# Creates and shows @c MainWindow.
    ///
    /// @param username  Active profile name.  Pass @c "guest" for an
    ///                  unauthenticated session that uses default paths.
    /// @param isAdmin   Whether @p username's profile has @c Profile::Role::Admin.
    ///                  Gates per-user recording access control: an admin's
    ///                  session browser/Analysis tab aggregate every known
    ///                  profile's recordings instead of being scoped to just
    ///                  their own, and a one-time flat-session-folder
    ///                  migration runs only when this is true.
    void initialize(const QString& username = "guest", bool isAdmin = false);

    /// @brief Saves settings and closes the window.
    ///
    /// Connected to @c QCoreApplication::aboutToQuit automatically — calling
    /// it manually is safe but unnecessary in normal usage.
    void shutdown();

    /// @returns A mutable reference to the current session's AppSettings.
    [[nodiscard]] AppSettings&       settings();

    /// @returns An immutable reference to the current session's AppSettings.
    [[nodiscard]] const AppSettings& settings() const;

    /// @returns The username of the currently active profile (@c "guest" if
    ///          no profile was selected at login).
    [[nodiscard]] QString            active_username()  const;

    /// @returns The application-wide TriggerManager, or @c nullptr before
    ///          initialize() has been called.
    [[nodiscard]] TriggerManager*    trigger_manager()  const;

    /// @returns The application-wide AudioManager.
    [[nodiscard]] AudioManager*      audio_manager()    const;

    /// @returns The application-wide VideoManager.
    [[nodiscard]] VideoManager*      video_manager()    const;

    /// @returns The application-wide RecordManager.
    [[nodiscard]] RecordManager*     record_manager()   const;

signals:
    /// Emitted at the end of initialize() when all subsystems are ready.
    void initialized();

    /// Emitted at the end of shutdown() after all resources are released.
    void shutdown_complete();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
