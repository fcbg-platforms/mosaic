#include "video/frame_timestamp_writer.hpp"

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
    std::atomic<int64_t> count{0};
    bool open{false};
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
    d->stream << "frame_id,elapsed_ns,wall_ns,hw_timestamp_ns\n";
    d->stream.flush();
    d->count.store(0, std::memory_order_relaxed);
    d->open = true;
    return true;
}

void FrameTimestampWriter::write(int64_t frameId, int64_t elapsedNs, int64_t wallNs,
                                 int64_t hwTimestampNs) {
    QMutexLocker lock(&d->mutex);
    if (!d->open) return;
    d->stream << frameId << ',' << elapsedNs << ',' << wallNs << ',' << hwTimestampNs << '\n';
    d->count.fetch_add(1, std::memory_order_relaxed);
}

void FrameTimestampWriter::stop() {
    QMutexLocker lock(&d->mutex);
    if (!d->open) return;
    d->stream.flush();
    d->file.close();
    d->open = false;
}

bool FrameTimestampWriter::is_open() const { return d->open; }
int64_t FrameTimestampWriter::frames_written() const {
    return d->count.load(std::memory_order_relaxed);
}

} // namespace mosaic
