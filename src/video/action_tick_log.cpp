#include "video/action_tick_log.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>

#include "utils/logger.hpp"

namespace mosaic {

struct ActionTickLog::Impl {
    QFile file;
    QTextStream stream;
    QElapsedTimer sinceFlush;
    int flushIntervalMs = 1000;
    int64_t rows        = 0;
    bool open           = false;
};

ActionTickLog::ActionTickLog() : d(std::make_unique<Impl>()) {}

ActionTickLog::~ActionTickLog() { close(); }

bool ActionTickLog::open(const QString& path) {
    close();
    d->file.setFileName(path);
    if (!d->file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        log_warning(QString("[ActionTickLog] Cannot write %1 (%2) — this recording's frames will "
                            "be aligned on arrival time instead of trigger ticks.")
                        .arg(path, d->file.errorString()));
        return false;
    }
    d->stream.setDevice(&d->file);
    d->stream << "tick,elapsed_ns,fired\n";
    d->stream.flush();
    d->rows = 0;
    d->sinceFlush.start();
    d->open = true;
    return true;
}

void ActionTickLog::append(int64_t tick, int64_t elapsedNs, int fired) {
    if (!d->open) return;
    d->stream << tick << ',' << elapsedNs << ',' << fired << '\n';
    ++d->rows;
    if (d->sinceFlush.elapsed() >= d->flushIntervalMs) {
        d->stream.flush();
        d->sinceFlush.restart();
    }
}

void ActionTickLog::close() {
    if (!d->open) return;
    d->stream.flush();
    d->file.close();
    d->open = false;
}

bool ActionTickLog::is_open() const { return d->open; }
int64_t ActionTickLog::rows_written() const { return d->rows; }

void ActionTickLog::set_flush_interval_ms(int ms) { d->flushIntervalMs = ms < 0 ? 0 : ms; }

} // namespace mosaic
