#pragma once
#include "core/settings.hpp"
#include "utils/ring_buffer.hpp"
#include "video/video_frame.hpp"
#include <QImage>
#include <QString>
#include <QThread>
#include <QVector>
#include <atomic>
#include <memory>

namespace mosaic {

class TriggerManager;

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
    // frameBuffer must outlive this object. triggerMgr, if non-null, must
    // outlive this object too — used to publish one LSL frame marker per
    // captured frame (see TriggerManager::publish_frame_marker()); pass
    // nullptr to skip this (e.g. when LSL support isn't built in).
    explicit VideoGrabber(int                                      cameraIndex,
                          const CameraParameters&                  params,
                          RingBuffer<std::shared_ptr<VideoFrame>>& frameBuffer,
                          TriggerManager*                          triggerMgr = nullptr,
                          QObject*                                 parent = nullptr);
    ~VideoGrabber() override;

    // Opens the camera device (Pylon) or prepares the stub generator.
    // Safe to call from the main thread before start().
    [[nodiscard]] bool open();
    void               close();

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

    [[nodiscard]] bool    is_open()          const;
    [[nodiscard]] int64_t frames_grabbed()   const;
    [[nodiscard]] int64_t frames_dropped()   const; // dropped due to full ring buffer
    [[nodiscard]] double  current_fps()      const;

    // elapsed_ns() of the most recently grabbed frame, or -1 if none yet.
    // Used by PerformanceMonitorW to show a live cross-camera skew estimate
    // (max - min across cameras) while recording — a coarse, once-per-second
    // signal distinct from SyncManifest's precise post-hoc report.
    [[nodiscard]] int64_t last_frame_elapsed_ns() const;

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

protected:
    void run() override;

private:
    // Writes exposure/gain/gamma/black-level/white-balance/auto-target/
    // digital-shift nodes from d->params onto the currently-open camera.
    // Shared by open() (initial configuration) and apply_live_params()
    // (re-applying after a UI edit) so the node-writing logic exists once.
    void apply_image_params();

    void run_pylon_loop();
    void run_stub_loop();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
