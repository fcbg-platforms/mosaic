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

class WavWriter {
public:
    WavWriter();
    ~WavWriter();

    [[nodiscard]] bool open(const QString& path,
                             int sampleRate,
                             int channels,
                             int bitsPerSample = 16);

    // Thread-safe. Returns false if not open or a disk error occurred.
    bool write(const char* data, qint64 bytes);

    // Finalises the file (fixes up the chunk sizes) and closes it.
    void close();

    [[nodiscard]] bool    is_open()        const;
    [[nodiscard]] int64_t bytes_written()  const;
    [[nodiscard]] double  duration_sec()   const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
