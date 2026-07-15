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

    [[nodiscard]] bool    is_open()        const;
    [[nodiscard]] int64_t frames_written() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
