#pragma once
#include <QImage>
#include <QString>
#include <QThread>
#include <QVector>
#include <atomic>
#include <memory>

#include "core/settings.hpp"
#include "utils/ring_buffer.hpp"
#include "video/video_frame.hpp"

namespace mosaic {

// One physically-connected camera found by VideoGrabber::enumerate_devices().
struct DiscoveredCamera {
    QString serialNumber;
    QString modelName;
    QString ipAddress;
};

// Grabs frames from one camera and pushes them into a shared ring buffer.
//
// When MOSAIC_HAVE_CAMERAS is defined: uses Basler Pylon SDK.
// Otherwise: emits a BGR8 test-pattern (colour bars) at the configured FPS.
//
// The grabber runs in a dedicated QThread. Call open() from the main thread
// before calling start(); the grab loop runs inside run().
//
// Frame timing uses elapsed_ns() (steady_clock) + wall_clock_ns() so
// downstream consumers can align to both an absolute wall timestamp and
// a drift-free monotonic timeline.

class VideoGrabber : public QThread {
    Q_OBJECT
   public:
    // frameBuffer must outlive this object.
    explicit VideoGrabber(int cameraIndex, const CameraParameters& params,
                          RingBuffer<std::shared_ptr<VideoFrame>>& frameBuffer,
                          QObject* parent = nullptr);
    ~VideoGrabber() override;

    // Opens the camera device (Pylon) or prepares the stub generator.
    // Safe to call from the main thread before start().
    [[nodiscard]] bool open();
    void close();

    // Requests that the subset of parameters safe to change on an
    // already-open, actively-grabbing camera (exposure, gain, gamma, black
    // level, white balance, auto-exposure target, digital shift) be
    // re-applied, without stopping or reopening the device. Reads current
    // values straight from the CameraParameters reference passed to the
    // constructor, so the caller just needs to have already mutated it
    // (e.g. via the settings UI) before calling this. Safe to call from any
    // thread — the actual node writes happen on the grab thread (shortly
    // after, between frames) rather than synchronously here, since Pylon's
    // camera/node-map access isn't documented as safe to use concurrently
    // with the grab thread's own RetrieveResult() calls. Structural
    // parameters (resolution, pixel format, frame rate, hardware trigger)
    // require a full close+reopen and are intentionally not touched here.
    // No-op if the device isn't open, or in stub builds.
    void apply_live_params();

    // Start / stop the grab loop (QThread::start / requestInterruption + wait).
    void start_grabbing();
    void stop_grabbing();

    // One-shot latch: the NEXT frame this grabber captures after this call
    // is emitted via calibration_frame_ready() at full resolution, in
    // addition to (not instead of) the normal ring-buffer push and the
    // downscaled preview_frame() signal. Used by room calibration, which
    // needs real, undownscaled pixels for accurate ChArUco corner detection
    // — preview_frame() is deliberately capped to 640×360 for the live
    // monitor and is not suitable for that. Safe to call from any thread.
    //
    // token is echoed back verbatim on calibration_frame_ready() so a caller
    // that issues one request per "shot" (e.g. RoomCalibrationW) can reject a
    // reply that arrives after it has already moved on to a later shot,
    // instead of relying only on timing (a delayed reply — e.g. from GigE
    // packet loss/resend — could otherwise be silently misattributed to the
    // wrong shot).
    void request_calibration_frame(uint64_t token = 0);

    // True only once the grab thread has actually reached Pylon's
    // StartGrabbing() (or, in stub builds, the equivalent point in
    // run_stub_loop()) — NOT merely once start_grabbing() has returned,
    // which only confirms the thread was scheduled to start, not that the
    // camera is actually listening for triggers yet. Used by
    // VideoManager::arm_and_fire_action_commands() to avoid firing the
    // first Action Command(s) before every armed camera is truly ready.
    [[nodiscard]] bool is_actually_grabbing() const;

    [[nodiscard]] bool is_open() const;
    [[nodiscard]] int64_t frames_grabbed() const;
    [[nodiscard]] int64_t frames_dropped() const; // dropped due to full ring buffer
    [[nodiscard]] double current_fps() const;

    // Cumulative GVSP-incomplete-frame count for this camera's whole open()
    // session — never reset, unlike the "N incomplete frame(s) in last 5s"
    // warning's own 5s-windowed counter (see run_pylon_loop()), which is
    // reset to 0 right after each log line. Distinct from frames_dropped(),
    // which is ring-buffer overflow (a full local buffer), not GigE packet
    // loss (a frame that started arriving and never completed). Used by
    // SessionHealthReport to surface this per-camera, post-recording,
    // instead of only as a periodically-reset log line.
    [[nodiscard]] int64_t incomplete_frames_total() const;

    // elapsed_ns() of the most recently grabbed frame, or -1 if none yet.
    // Used by PerformanceMonitorW to show a live cross-camera skew estimate
    // (max - min across cameras) while recording — a coarse, once-per-second
    // signal distinct from SyncManifest's precise post-hoc report.
    [[nodiscard]] int64_t last_frame_elapsed_ns() const;

    // Set at open() time only when hwTriggerEnabled && hwTriggerSource ==
    // "Action1" AND this firmware exposed writable ActionSelector/
    // ActionDeviceKey/ActionGroupKey/ActionGroupMask nodes when probed. When
    // false (either because Action1 wasn't requested, or the probe failed
    // and this camera fell back to free-run for the session), callers must
    // not include this grabber in a GigE Vision Action Command target list
    // (see gige_action_command.hpp).
    [[nodiscard]] bool action_command_ready() const;
    [[nodiscard]] uint32_t action_device_key() const;
    [[nodiscard]] QString action_broadcast_address() const;

    // The fps this grabber was configured with (CameraParameters::fps).
    // Plain struct read — always available even in stub builds. Used by
    // VideoManager to pick a shared continuous Action Command firing rate
    // across every Action1-armed camera without needing VideoSettings
    // threaded through start_preview() (which doesn't take one).
    [[nodiscard]] double configured_fps() const;

    // The camera's real, measured ResultingFrameRate — refreshed at open()
    // time, after every apply_live_params() call, and periodically (every
    // ~2s) while grabbing (see refresh_achievable_fps()), so it keeps
    // improving over the life of a session rather than staying pinned at
    // whatever was measured first. A reading is only trusted once the
    // camera has been genuinely grabbing for a little while (see
    // is_achievable_fps_measurement_warmed_up() in gige_action_command.hpp)
    // — not filtered by its own magnitude, since real room-11 data showed a
    // magnitude floor can't tell a premature reading apart from a real,
    // stable, but disappointingly low one (e.g. auto-exposure converged
    // near its ceiling in a dim room). So this can still be -1.0 (never yet
    // measured) for a few seconds after open() even though
    // refresh_achievable_fps() already ran — expected, not a bug; a
    // trustworthy reading lands as soon as the warm-up window passes. May be
    // lower than configured_fps() if GigE bandwidth/exposure/ROI can't
    // sustain the requested rate (see the "Requested X fps but the camera
    // can only sustain ~Y fps" warning). Preferred over
    // configured_fps() when picking a shared Action Command firing rate, so
    // the ticker doesn't trigger a camera faster than it can actually
    // process — that would reproduce the same GigE packet-loss failure mode
    // already seen from over-requested free-run fps, just self-inflicted by
    // the trigger cadence instead. Returns -1.0 if never measured (stub
    // builds, or the node was unavailable) — callers should fall back to
    // configured_fps() in that case.
    [[nodiscard]] double achievable_fps() const;

    // Lists every Pylon-visible GigE Vision device currently reachable on the
    // network, independent of any opened camera/session. Used by the "Discover
    // cameras" UI action to auto-fill camera cards with real serial numbers
    // instead of requiring the user to hand-type them via an external tool.
    // Returns an empty list in stub builds (MOSAIC_HAVE_CAMERAS not defined).
    [[nodiscard]] static QVector<DiscoveredCamera> enumerate_devices();

   signals:
    void opened(int cameraIndex, int width, int height, double fps);
    void closed(int cameraIndex);
    void frame_dropped(int cameraIndex, int64_t frameId);
    void grab_error(int cameraIndex, QString message);
    // Throttled preview at ~15 fps — used for live QML display only.
    void preview_frame(int cameraIndex, QImage frame);
    // Emitted once, at full resolution (unlike preview_frame(), which is
    // capped to 640×360), in response to a preceding
    // request_calibration_frame() call. QImage (not VideoFrame) to match
    // preview_frame()'s existing, already-proven-safe cross-thread payload
    // convention rather than introducing a new custom-struct Qt meta-type.
    // token is whatever was passed to request_calibration_frame().
    void calibration_frame_ready(int cameraIndex, QImage frame, uint64_t token);

    // Emitted once from open(), only when hwTriggerEnabled &&
    // hwTriggerSource == "Action1" was requested — i.e. only when the user
    // actually asked for GigE Vision Action Command triggering, so cameras
    // not using it never pay the extra node probe or generate UI noise. Lets
    // the HW Trigger tab show a definitive per-camera "Action-command
    // supported: yes/no" readout without inspecting logs.
    void action_command_capability(int cameraIndex, bool supported);

   protected:
    void run() override;

   private:
    // Writes exposure/gain/gamma/black-level/white-balance/auto-target/
    // digital-shift nodes from d->params onto the currently-open camera.
    // Shared by open() (initial configuration) and apply_live_params()
    // (re-applying after a UI edit) so the node-writing logic exists once.
    void apply_image_params();

    // Re-reads d->resultingFps and re-checks the "requested fps not
    // achievable" warning — see the .cpp definition's doc comment for why
    // this must run again after a live parameter change, not just at
    // open() time. Called from open() and from run_pylon_loop()'s
    // live-apply branch, right after apply_image_params().
    void refresh_achievable_fps();

    void run_pylon_loop();
    void run_stub_loop();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
