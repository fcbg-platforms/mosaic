#include "trigger/trigger_recorder.hpp"

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>

#include "utils/timestamp.hpp"

namespace mosaic {

struct TriggerRecorder::Impl {
    QMutex mutex;
    QFile file;
    bool open{false};
    int64_t startNs{0};
};

TriggerRecorder::TriggerRecorder() : d(std::make_unique<Impl>()) {}
TriggerRecorder::~TriggerRecorder() { stop(); }

bool TriggerRecorder::start(const QString& path) {
    QMutexLocker lock(&d->mutex);
    if (d->open) stop();

    d->file.setFileName(path);
    if (!d->file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) return false;

    d->startNs = elapsed_ns();
    d->open    = true;

    // Write CSV header
    QTextStream out(&d->file);
    out << "elapsed_ms,elapsed_ns,wall_clock,source,label,value\n";
    return true;
}

void TriggerRecorder::record_event(const TriggerEvent& ev) {
    QMutexLocker lock(&d->mutex);
    if (!d->open) return;

    // elapsed_ms is recording-relative (zeroed at start(), a DIFFERENT
    // zero-point than timestamps_camN.csv's app-launch-relative elapsed_ns)
    // — display-only, not safe for cross-file alignment. elapsed_ns below is
    // ev.timestampNs raw and unmodified, sharing the exact same elapsed_ns()
    // origin as every frame timestamp — that's the column to join on.
    const double elapsedMs  = static_cast<double>(ev.timestampNs - d->startNs) / 1e6;
    const QString wallClock = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");

    QTextStream out(&d->file);
    out << QString::number(elapsedMs, 'f', 3) << "," << ev.timestampNs << "," << wallClock << ","
        << ev.source << ","
        << "\"" << QString(ev.label).replace('"', "\"\"") << "\","
        << QString::number(ev.value, 'f', 6) << "\n";
    out.flush();
}

void TriggerRecorder::stop() {
    QMutexLocker lock(&d->mutex);
    if (!d->open) return;
    d->file.close();
    d->open = false;
}

bool TriggerRecorder::is_recording() const {
    QMutexLocker lock(&d->mutex);
    return d->open;
}

} // namespace mosaic
