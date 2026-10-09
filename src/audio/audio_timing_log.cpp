#include "audio/audio_timing_log.hpp"

#include <QFileInfo>

namespace mosaic {

namespace {
// Every 20 buffers (a fraction of a second to a couple of seconds, depending
// on how often the audio backend delivers), so a crash loses little.
constexpr int kFlushEveryRows = 20;
} // namespace

QString AudioTimingLog::path_for(const QString& wavPath) {
    const QFileInfo fi(wavPath);
    return fi.path() + "/" + fi.completeBaseName() + ".timing.csv";
}

bool AudioTimingLog::open(const QString& path) {
    close();
    file_.setFileName(path);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    file_.write("sample_count,elapsed_ns\n");
    rowsSinceFlush_ = 0;
    return true;
}

void AudioTimingLog::record(int64_t sampleFrames, int64_t elapsedNs) {
    if (!file_.isOpen()) {
        return;
    }
    file_.write(QByteArray::number(static_cast<qlonglong>(sampleFrames)) + ',' +
                QByteArray::number(static_cast<qlonglong>(elapsedNs)) + '\n');
    if (++rowsSinceFlush_ >= kFlushEveryRows) {
        file_.flush();
        rowsSinceFlush_ = 0;
    }
}

void AudioTimingLog::close() {
    if (file_.isOpen()) {
        file_.close();
    }
}

} // namespace mosaic
