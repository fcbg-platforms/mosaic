#pragma once
#include <QString>
#include <cstdint>
#include <memory>

namespace mosaic {

// Writes one CSV row per grabbed frame:
//   frame_id, elapsed_ns, wall_ns, hw_timestamp_ns
//
// hw_timestamp_ns is the camera's own hardware chunk timestamp (0 if
// unavailable) — see VideoFrame::hwTimestampNs for what it can and cannot
// be used for.
//
// Thread-safe: write() is called from the grabber thread while the main
// thread may call is_open() / frames_written() concurrently.
// The file is flushed and closed in stop() from whatever thread calls it.
//
// Rows also reach the file while recording — at most one flush interval
// (1 s by default) behind — so a crash leaves a CSV that is complete up to
// shortly before it, not one that stops wherever the stream buffer last
// happened to fill. It pairs with the fragmented MP4 the encoder writes, which
// reaches the disk in ~2 s fragments, so after a crash the CSV and the video
// can end a second or two apart, in either direction. Readers match frames by
// row (SyncManifest reads only the CSV; run_sync_repair freezes on the last
// frame if the video runs out first), so a ragged end costs those last
// seconds, nothing earlier.

class FrameTimestampWriter {
   public:
    FrameTimestampWriter();
    ~FrameTimestampWriter();

    // Opens the CSV and writes the header row.
    [[nodiscard]] bool start(const QString& path);

    // Appends one row. Thread-safe.
    void write(int64_t frameId, int64_t elapsedNs, int64_t wallNs, int64_t hwTimestampNs);

    // Flushes and closes the file.
    void stop();

    // How long write() may hold rows before flushing them to the file.
    // Default 1000 ms; 0 flushes every row. Exposed for tests.
    void set_flush_interval_ms(int ms);

    [[nodiscard]] bool is_open() const;
    [[nodiscard]] int64_t frames_written() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
