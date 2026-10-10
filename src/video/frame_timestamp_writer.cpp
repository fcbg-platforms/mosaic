#include "video/frame_timestamp_writer.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>
#include <atomic>

#include "utils/logger.hpp"

namespace mosaic {

struct FrameTimestampWriter::Impl {
    QFile file;
    QTextStream stream;
    QMutex mutex;
    bool open{false};
    QElapsedTimer sinceFlush;
    int flushIntervalMs{1000};
};

FrameTimestampWriter::FrameTimestampWriter() : d(std::make_unique<Impl>()) {}

FrameTimestampWriter::~FrameTimestampWriter() { stop(); }

bool FrameTimestampWriter::start(const QString& path) {
    QMutexLocker lock(&d->mutex);
    if (d->open) stop();

    d->file.setFileName(path);
    if (!d->file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        log_error(QString("[FrameTimestampWriter] Cannot open: %1 — %2")
                      .arg(path, d->file.errorString()));
        return false;
    }

    d->stream.setDevice(&d->file);
    d->stream << "frame_id,elapsed_ns,wall_ns,hw_timestamp_ns,exposure_us\n";
    d->stream.flush();
    d->sinceFlush.start();
    d->open = true;
    return true;
}

void FrameTimestampWriter::write(int64_t frameId, int64_t elapsedNs, int64_t wallNs,
                                 int64_t hwTimestampNs, double exposureUs) {
    QMutexLocker lock(&d->mutex);
    if (!d->open) return;
    d->stream << frameId << ',' << elapsedNs << ',' << wallNs << ',' << hwTimestampNs << ',';
    // Fixed notation: QTextStream's default would write a 1 s exposure as
    // "1e+06".
    if (exposureUs >= 0.0) d->stream << QString::number(exposureUs, 'f', 1);
    d->stream << '\n';
    // QTextStream::flush() also flushes the QFile underneath, so the rows
    // reach the OS — which keeps them through a crash of this process.
    if (d->sinceFlush.elapsed() >= d->flushIntervalMs) {
        d->stream.flush();
        d->sinceFlush.restart();
    }
}

void FrameTimestampWriter::set_flush_interval_ms(int ms) {
    QMutexLocker lock(&d->mutex);
    d->flushIntervalMs = ms < 0 ? 0 : ms;
}

void FrameTimestampWriter::stop() {
    QMutexLocker lock(&d->mutex);
    if (!d->open) return;
    d->stream.flush();
    d->file.close();
    d->open = false;
}

bool FrameTimestampWriter::is_open() const { return d->open; }
} // namespace mosaic
