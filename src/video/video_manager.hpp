#pragma once
#include <QImage>
#include <QObject>
#include <memory>

#include "core/settings.hpp"
#include "video/video_frame.hpp"

namespace mosaic {

/// @brief Orchestrates one VideoGrabber + VideoEncoder pair per configured camera.
///
/// VideoManager owns the complete video pipeline for a session.  It matches
/// the lifecycle of the cameras: open them once at startup, then start/stop
/// recording repeatedly without reopening.
///
/// @par Thread model
/// Each camera runs two background threads:
/// - **VideoGrabber** — grabs frames from Pylon (or generates test patterns).
/// - **VideoEncoder** — encodes frames via FFmpeg and writes the timestamp CSV.
///
/// Frames are passed between threads through a lock-free SPSC RingBuffer.
/// All *public* methods of VideoManager itself must be called from the main
/// thread.
///
/// @par Lifecycle
/// @code{.cpp}
/// VideoManager vm;
/// int opened = vm.open(settings.video);   // opens cameras once
///
/// // Start/stop can repeat throughout the session
/// vm.start("recordings/2026-06-04_14-32-05", "video", settings.video);
/// // ... recording in progress ...
/// vm.stop();   // blocks until all encoders have flushed and closed their files
///
/// vm.close();  // release camera handles (called by destructor)
/// @endcode
///
/// @see VideoGrabber, VideoEncoder, FrameTimestampWriter, RecordManager
class VideoManager : public QObject {
    Q_OBJECT
   public:
    explicit VideoManager(QObject* parent = nullptr);
    ~VideoManager() override;

    /// @brief Opens camera devices (or stub generators) for all configured cameras.
    ///
    /// @param settings  Video settings from AppSettings.  The @c cameras vector
    ///                  determines how many devices are opened.
    /// @returns         The number of cameras successfully opened.  May be less
    ///                  than @c settings.cameras.size() if some devices fail.
    int open(const VideoSettings& settings);

    /// @brief Closes all open camera handles.
    ///
    /// Calls stop() first if a recording is active.
    void close();

    /// @brief Starts the grab loop for all cameras without creating encoders.
    ///
    /// Call this after open() to see live preview frames in the QML monitor
    /// before any recording session begins.  Safe to call if grabbers are
    /// already running (no-op for those cameras).
    void start_preview();

    /// @brief Starts grabbing and encoding for all open cameras.
    ///
    /// Output files:
    /// - @c \<sessionDir\>/\<videoBasename\>_N.mp4
    /// - @c \<sessionDir\>/timestamps_camN.csv
    ///
    /// @param sessionDir    Absolute path to the session folder (must exist).
    /// @param videoBasename Basename for video files (e.g. @c "video").
    /// @param settings      Video settings (codec, preset, per-camera FPS/resolution).
    void start(const QString& sessionDir, const QString& videoBasename,
               const VideoSettings& settings);

    /// @brief Stops all grabbers and encoders, flushing and closing every file.
    ///
    /// Blocks until all encoder threads have exited (up to 10 s per camera).
    void stop();

    /// @brief Re-applies exposure/gain/gamma/black-level/white-balance/
    /// auto-target/digital-shift for one camera to already-open hardware,
    /// without stopping or reopening it.
    ///
    /// Call this after the caller has mutated the corresponding
    /// CameraParameters in place (e.g. via the settings UI). Structural
    /// parameters (resolution, pixel format, frame rate, hardware trigger)
    /// still require a full close()+open() to take effect and are not
    /// touched by this call.
    ///
    /// @param configIndex  Position in the *configured* settings.cameras
    ///                     array (VideoManager::CameraUnit::configIndex),
    ///                     not the camera's position among successfully
    ///                     opened units. No-op if no open unit matches.
    void apply_live_params(int configIndex);

    /// @brief Requests one full-resolution frame from the given camera for
    /// room (extrinsic) calibration — see VideoGrabber::request_calibration_frame().
    /// Delivered asynchronously via calibration_frame_ready(), echoing token
    /// back verbatim so a caller that requests one frame per "shot" can
    /// reject a reply for a shot it has already moved past. No-op if no
    /// open unit matches configIndex.
    void request_calibration_frame(int configIndex, uint64_t token = 0);

    /// @returns @c true while a recording session is active.
    [[nodiscard]] bool is_recording() const;

    /// @returns @c true while a live preview session is active (started via
    /// start_preview(), not yet stopped by start()/close()). Used to guard
    /// actions that would race with a running ActionCommandTicker, which
    /// touches Pylon's CTlFactory from a background thread for as long as
    /// either preview or recording keeps it alive.
    [[nodiscard]] bool is_previewing() const;

    /// @returns The number of cameras that were successfully opened.
    [[nodiscard]] int camera_count() const;

    /// @returns Total frames encoded across all cameras since the last start().
    [[nodiscard]] int64_t total_frames_encoded() const;

    /// @returns Total frames dropped (ring buffer overflow) since last start().
    [[nodiscard]] int64_t total_frames_dropped() const;

    /// @brief Per-camera real-time performance snapshot.
    struct CameraStats {
        double fps                 = 0.0; ///< Measured grab rate (frames/s).
        int64_t framesGrabbed      = 0;   ///< Total frames grabbed since start().
        int64_t framesEncoded      = 0;   ///< Total frames encoded since start().
        int64_t framesDropped      = 0;   ///< Frames lost to ring-buffer overflow.
        int ringFillPct            = 0;   ///< Ring buffer fill level, 0–100.
        bool grabberRunning        = false;
        int64_t lastFrameElapsedNs = -1; ///< elapsed_ns() of the most recent frame, -1 if none yet.
        double configuredFps       = 0.0; ///< VideoGrabber::configured_fps().
        double achievableFps = -1.0; ///< VideoGrabber::achievable_fps(), -1 = not yet measured.
        int64_t incompleteFrames =
            0; ///< VideoGrabber::incomplete_frames_total() (GVSP packet loss).
    };

    /// @brief Returns a performance snapshot for one camera.
    ///
    /// @param index  Zero-based camera index.  Returns a zeroed struct if out of range.
    [[nodiscard]] CameraStats camera_stats(int index) const;

    /// @returns The number of GigE Vision Action Command ticks fired during
    /// this camera group's most recently *attempted* arm_and_fire_action_commands()
    /// call (recording or preview, whichever ran most recently), or -1 if
    /// that call didn't use Action1 triggering at all — reset to -1 at the
    /// start of every arm_and_fire_action_commands() call, before a ticker
    /// is even created, so a session that doesn't use Action1 never reports
    /// a stale count left over from an earlier session that did. Snapshotted
    /// in stop_action_ticker() right before the ticker is destroyed, since
    /// the live tick count itself is otherwise lost the instant the ticker
    /// object goes away. This is one shared, group-wide count — see
    /// camera_action_command_ready() for whether it's even meaningful for a
    /// particular camera. Combined with a camera's own frames_grabbed() by
    /// SessionHealthReport to report how many trigger broadcasts a camera
    /// missed, not just how many frames it captured.
    [[nodiscard]] int64_t action_ticks_fired() const;

    /// @returns Whether this camera was Action1-ready (and therefore part of
    /// the Action Command target group whose shared tick count
    /// action_ticks_fired() reports) as of its last open()/probe. False for
    /// out-of-range indices and for any camera not opened for Action1
    /// triggering, in which case action_ticks_fired()'s count is unrelated
    /// to it and callers must not apply it to that camera (a session can mix
    /// Action1-armed and free-running cameras).
    [[nodiscard]] bool camera_action_command_ready(int index) const;

    /// @brief One camera's final counters for the recording that just ended.
    ///
    /// Deliberately a *snapshot*, not a live read: every live counter above is
    /// zeroed again by VideoGrabber::start_grabbing() the moment preview
    /// resumes, and Application::initialize() connects its own preview-restart
    /// handler to RecordManager::recording_stopped *before* MainWindow exists,
    /// so it always runs first. Anything reading camera_stats() from a
    /// recording_stopped handler therefore sees zeros for every camera. Use
    /// last_recording_snapshot() instead for post-recording reporting.
    struct RecordingCameraSnapshot {
        /// Position in settings.cameras this unit was opened from — NOT its
        /// position in the internal (compacted) unit list. See
        /// CameraUnit::configIndex.
        int configIndex          = 0;
        int64_t framesGrabbed    = 0;
        int64_t framesEncoded    = 0;
        int64_t framesDropped    = 0;
        int64_t incompleteFrames = 0;
        double configuredFps     = 0.0;
        double achievableFps     = -1.0; ///< -1 = not measured
        bool actionCommandReady  = false;
    };

    /// @returns One entry per camera that was actually open for the recording
    /// that just ended, captured at the end of stop(). Cameras that were
    /// configured but never opened (duplicate serial, failed open(), dead
    /// link) have no entry at all — callers wanting to report them must
    /// reconcile against settings.cameras via each entry's configIndex.
    /// Empty until the first stop().
    [[nodiscard]] const std::vector<RecordingCameraSnapshot>& last_recording_snapshot() const;

    /// @returns action_ticks_fired() as captured at the end of the recording
    /// that just ended, for the same reason last_recording_snapshot() exists:
    /// the live value is reset to -1 by the preview restart that follows
    /// recording_stopped. -1 if that recording didn't use Action1 triggering.
    [[nodiscard]] int64_t last_recording_action_ticks() const;

    /// @brief Discards the previous recording's snapshot.
    ///
    /// Must be called when a recording *starts*, not only relied on being
    /// overwritten when one stops: stop() early-returns unless this manager is
    /// actually recording, and a session can end without it ever having
    /// started (video disabled for the session, or start() bailing because no
    /// camera is open). Without this, such a session's health report would
    /// present the *previous* recording's counters as its own.
    void clear_recording_snapshot();

   signals:
    /// Emitted on the main thread when a camera device is successfully opened.
    void camera_opened(int cameraIndex, int width, int height, double fps);

    /// Emitted when a camera device is closed.
    void camera_closed(int cameraIndex);

    /// Emitted each time a frame is dropped because the ring buffer was full.
    /// @param cameraIndex  Which camera dropped the frame.
    /// @param frameId      The frame counter value of the dropped frame.
    void frame_dropped(int cameraIndex, int64_t frameId);

    /// Emitted when a camera grab or encode error occurs.
    void camera_error(int cameraIndex, QString message);

    /// Emitted when all encoders have finished (files closed and flushed).
    void recording_stopped();

    /// Throttled (~15 fps) BGR preview for live QML display.
    void frame_preview(int cameraIndex, QImage frame);

    /// Full-resolution frame delivered in response to request_calibration_frame().
    /// token is whatever was passed to request_calibration_frame().
    void calibration_frame_ready(int cameraIndex, QImage frame, uint64_t token);

    /// Passthrough of VideoGrabber::action_command_capability() — reports
    /// whether a camera's firmware supports GigE Vision Action Command
    /// triggering, once probed at open() time. Only fires for cameras that
    /// actually requested Action1 triggering (hwTriggerEnabled &&
    /// hwTriggerSource == "Action1").
    void action_command_capability(int cameraIndex, bool supported);

    /// Passthrough of VideoGrabber::achievable_fps_changed() — the camera's
    /// own measured ResultingFrameRate, for the achievable-rate readout beside
    /// the exposure controls. `cameraIndex` is a *config* index (a position in
    /// settings.cameras), like every other signal here and unlike
    /// camera_stats()'s unit index. A closing camera reports -1.0 so a stale
    /// reading can't outlive the session that measured it.
    void achievable_fps_changed(int cameraIndex, double fps);

   private slots:
    void on_encoder_stopped(int cameraIndex, int64_t frames);

   private:
    // Arms every unit's grabber (start_grabbing()) first, then, for every
    // Action1-ready unit, starts a background ActionCommandTicker that
    // fires one GigE Vision IssueActionCommand() per frame, continuously,
    // for as long as the ticker runs — shared by start_preview() and
    // start(), since a camera configured for Action1 hangs identically
    // (produces zero frames) in either mode until action commands start
    // arriving. See gige_action_command.hpp.
    void arm_and_fire_action_commands();

    // Stops and destroys the current ActionCommandTicker, if any. Called
    // from every path that stops grabbers (stop(), close(), and start()'s
    // preview-teardown step) so the ticker never outlives the cameras it's
    // firing at. No-op if no ticker is running.
    void stop_action_ticker();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
