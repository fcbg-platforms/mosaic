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
    explicit VideoGrabber(int                                      cameraIndex,
                          const CameraParameters&                  params,
                          RingBuffer<std::shared_ptr<VideoFrame>>& frameBuffer,
                          QObject*                                 parent = nullptr);
    ~VideoGrabber() override;

    // Opens the camera device (Pylon) or prepares the stub generator.
    // Safe to call from the main thread before start().
    [[nodiscard]] bool open();
    void               close();

    // Start / stop the grab loop (QThread::start / requestInterruption + wait).
    void start_grabbing();
    void stop_grabbing();

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

protected:
    void run() override;

private:
    void run_pylon_loop();
    void run_stub_loop();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
