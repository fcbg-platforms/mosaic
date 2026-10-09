#pragma once
#include <QFile>
#include <QString>
#include <cstdint>

namespace mosaic {

/// Writes "<name>.timing.csv" beside a recorded WAV: after each buffer of
/// audio arrives, the number of sample frames written so far and the
/// elapsed_ns() clock at arrival (the clock video frames are stamped with).
/// Analysis fits a line under these points to place every audio sample on
/// the video clock, including the sound card's clock drift (see
/// analysis/conversation/timing.py).
class AudioTimingLog {
   public:
    AudioTimingLog() = default;
    ~AudioTimingLog() { close(); }
    AudioTimingLog(const AudioTimingLog&)            = delete;
    AudioTimingLog& operator=(const AudioTimingLog&) = delete;

    /// "dir/audio.wav" -> "dir/audio.timing.csv".
    [[nodiscard]] static QString path_for(const QString& wavPath);

    /// Creates the file and writes its header. False if it cannot be created.
    bool open(const QString& path);
    /// One row: total sample frames written so far, and the arrival time.
    void record(int64_t sampleFrames, int64_t elapsedNs);
    void close();
    [[nodiscard]] bool is_open() const { return file_.isOpen(); }

   private:
    QFile file_;
    int rowsSinceFlush_ = 0;
};

} // namespace mosaic
