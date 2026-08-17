#include "utils/logger.hpp"

#include <QDebug>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>

#include "utils/timestamp.hpp"

namespace mosaic {

const char* log_level_label(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO ";
        case LogLevel::Warning:
            return "WARN ";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Critical:
            return "CRIT ";
    }
    return "?????";
}

struct Logger::Impl {
    QMutex mutex;
    LogLevel minLevel{LogLevel::Debug};
    QFile logFile;
};

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::Logger() : QObject(nullptr), d(std::make_unique<Impl>()) {}
Logger::~Logger() = default; // Impl is complete here — unique_ptr can call delete

void Logger::log(LogLevel level, const QString& message, std::source_location loc) {
    if (static_cast<int>(level) < static_cast<int>(d->minLevel)) return;

    const QString ts     = wall_clock_string();
    const QString locStr = QString("%1:%2").arg(loc.file_name()).arg(loc.line());
    const QString fullLine =
        QString("[%1] [%2] [%3] %4").arg(ts, log_level_label(level), locStr, message);

    {
        QMutexLocker lock(&d->mutex);
        if (d->logFile.isOpen()) {
            QTextStream stream(&d->logFile);
            stream << fullLine << '\n';
            // Flush immediately so the file is always readable on crash.
            stream.flush();
        }
    }

#ifndef QT_NO_DEBUG
    qDebug().noquote() << fullLine;
#endif

    emit entry_added(static_cast<int>(level), ts, locStr, message);
}

void Logger::set_min_level(LogLevel level) {
    QMutexLocker lock(&d->mutex);
    d->minLevel = level;
}

bool Logger::open_log_file(const QString& path) {
    QMutexLocker lock(&d->mutex);
    if (d->logFile.isOpen()) d->logFile.close();
    d->logFile.setFileName(path);
    return d->logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
}

void Logger::close_log_file() {
    QMutexLocker lock(&d->mutex);
    d->logFile.close();
}

void log_trace(const QString& msg, std::source_location loc) {
    Logger::instance().log(LogLevel::Trace, msg, loc);
}
void log_debug(const QString& msg, std::source_location loc) {
    Logger::instance().log(LogLevel::Debug, msg, loc);
}
void log_info(const QString& msg, std::source_location loc) {
    Logger::instance().log(LogLevel::Info, msg, loc);
}
void log_warning(const QString& msg, std::source_location loc) {
    Logger::instance().log(LogLevel::Warning, msg, loc);
}
void log_error(const QString& msg, std::source_location loc) {
    Logger::instance().log(LogLevel::Error, msg, loc);
}
void log_critical(const QString& msg, std::source_location loc) {
    Logger::instance().log(LogLevel::Critical, msg, loc);
}

} // namespace mosaic
