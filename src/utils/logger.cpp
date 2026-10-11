#include "utils/logger.hpp"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    std::function<void(const QString&, const QString&, const QString&)> problemSink;
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

    if (static_cast<int>(level) >= static_cast<int>(LogLevel::Warning)) {
        std::function<void(const QString&, const QString&, const QString&)> sink;
        {
            QMutexLocker lock(&d->mutex);
            sink = d->problemSink;
        }
        if (sink) {
            sink(QString::fromLatin1(log_level_label(level)).trimmed(), locStr, message);
        }
    }

    emit entry_added(static_cast<int>(level), ts, locStr, message);
}

void Logger::set_min_level(LogLevel level) {
    QMutexLocker lock(&d->mutex);
    d->minLevel = level;
}

bool Logger::open_log_file(const QString& path) {
    QMutexLocker lock(&d->mutex);
    if (d->logFile.isOpen()) d->logFile.close();
    // Keep the previous run's log (the one that matters after a crash)
    // instead of truncating it away.
    const QFileInfo info(path);
    bool appendInstead = false;
    if (info.exists() && info.size() > 0) {
        // Set aside first, then the copy before it goes. If the rename fails
        // (another MOSAIC on this profile has the file open), keep writing at
        // its end rather than wiping a log that is still in use.
        const QString previous = info.dir().filePath(info.completeBaseName() + ".previous.log");
        const QString older    = previous + ".old";
        QFile::remove(older);
        const bool hadPrevious = QFile::exists(previous) && QFile::rename(previous, older);
        if (QFile::rename(path, previous)) {
            QFile::remove(older);
        } else {
            if (hadPrevious) {
                QFile::rename(older, previous);
            }
            appendInstead = true;
        }
    }
    d->logFile.setFileName(path);
    const auto mode = QIODevice::WriteOnly | QIODevice::Text |
                      (appendInstead ? QIODevice::Append : QIODevice::Truncate);
    return d->logFile.open(mode);
}

void Logger::set_problem_sink(
    std::function<void(const QString& level, const QString& source, const QString& message)> sink) {
    QMutexLocker lock(&d->mutex);
    d->problemSink = std::move(sink);
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
