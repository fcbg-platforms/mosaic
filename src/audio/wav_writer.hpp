#pragma once
#include <QString>
#include <memory>

namespace mosaic {

// Writes raw PCM audio to a WAV (RIFF) file.
// Thread-safe: write() can be called from the audio capture thread while
// the main thread holds a reference to this object.
//
// Usage:
//   WavWriter w;
//   w.open(path, 44100, 2, 16);
//   w.write(buf, bytes);   // many times
//   w.close();             // seeks back to fix RIFF / data chunk sizes
//
// The chunk sizes are also brought up to date while recording, at most one
// header-update interval (1 s by default) behind. A WAV whose sizes still say
// 0 — what a crash used to leave — reads as empty in most tools even though
// every sample is on disk; this way it declares what was written up to
// shortly before the crash.

class WavWriter {
   public:
    WavWriter();
    ~WavWriter();

    [[nodiscard]] bool open(const QString& path, int sampleRate, int channels,
                            int bitsPerSample = 16);

    // Thread-safe. Returns false if not open or a disk error occurred.
    bool write(const char* data, qint64 bytes);

    // Finalises the file (fixes up the chunk sizes) and closes it.
    void close();

    // How long write() may leave the header's chunk sizes stale. Default
    // 1000 ms; 0 updates them on every write. Exposed for tests.
    void set_header_update_interval_ms(int ms);

    [[nodiscard]] bool is_open() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
