#pragma once
#include <QString>
#include <cstdint>
#include <memory>

namespace mosaic {

// Publishes per-frame timing markers to a Lab Streaming Layer outlet so that
// external tools (EEG, eye-trackers, physiological recorders) can align their
// data streams to MOSAIC's timeline.
//
// Stream layout (3 channels, type "VideoFrames"):
//   ch 0  camera_index   (float64)
//   ch 1  frame_id       (float64)
//   ch 2  elapsed_ns     (float64)
//
// Requires MOSAIC_HAVE_LSL at compile time; a no-op stub is compiled otherwise.

class LslOutlet {
public:
    LslOutlet();
    ~LslOutlet();

    // Creates the LSL outlet. outletName is the stream name visible to other apps.
    // Returns false if MOSAIC_HAVE_LSL is not defined.
    [[nodiscard]] bool open(const QString& outletName, double nominalSampleRate = 30.0);
    void               close();

    [[nodiscard]] bool is_open() const;

    // Push one video-frame marker. Thread-safe — safe to call from the grabber thread.
    void push_frame(int cameraIndex, int64_t frameId, int64_t elapsedNs);

    // Push a free-form string event (for session start/stop, trigger labels, etc.).
    void push_event(const QString& label);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
