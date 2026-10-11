#include "utils/problem_journal.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QSaveFile>
#include <QTimeZone>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace mosaic {

QJsonObject ProblemEntry::to_json() const {
    QJsonObject o{
        {"time_utc", timeUtc.toString(Qt::ISODateWithMs)},
        {"level", level},
        {"kind", kind},
        {"profile", profile},
        {"version", version},
        {"run", runId},
        {"message", message},
    };
    if (!source.isEmpty()) {
        o["source"] = source;
    }
    if (repeat > 1) {
        o["repeat"] = repeat;
    }
    return o;
}

std::optional<ProblemEntry> ProblemEntry::from_json(const QJsonObject& o) {
    ProblemEntry e;
    e.timeUtc = QDateTime::fromString(o["time_utc"].toString(), Qt::ISODateWithMs);
    if (!e.timeUtc.isValid() || !o["message"].isString()) {
        return std::nullopt;
    }
    e.timeUtc.setTimeZone(QTimeZone::utc());
    e.level   = o["level"].toString();
    e.kind    = o["kind"].toString("log");
    e.profile = o["profile"].toString();
    e.version = o["version"].toString();
    e.runId   = o["run"].toString();
    e.source  = o["source"].toString();
    e.message = o["message"].toString();
    e.repeat  = std::max(1, o["repeat"].toInt(1));
    return e;
}

bool journal_should_write(int occurrence) {
    if (occurrence < 1) {
        return false;
    }
    if (occurrence <= 3) {
        return true;
    }
    int n = occurrence; // a power of ten?
    while (n % 10 == 0) {
        n /= 10;
    }
    return n == 1;
}

ProblemJournal::ProblemJournal(QString dir, qint64 maxBytes)
    : dir_(std::move(dir)), maxBytes_(maxBytes) {}

QString ProblemJournal::path() const { return dir_ + "/problems.jsonl"; }
QString ProblemJournal::rotated_path() const { return dir_ + "/problems.1.jsonl"; }
QString ProblemJournal::crash_dir() const { return dir_ + "/crashes"; }
QString ProblemJournal::marker_dir() const { return dir_ + "/running"; }
QString ProblemJournal::pending_dir() const { return dir_ + "/running/unreported"; }

namespace {

// A profile name or run id, safe in a file name.
QString safe_name(QString s) {
    for (QChar& c : s) {
        if (!c.isLetterOrNumber() && c != '-' && c != '_') {
            c = '_';
        }
    }
    return s.isEmpty() ? QStringLiteral("unknown") : s;
}

// Messages from one log call that differ only in their numbers (a frame
// count, a camera index) are the same problem.
QString without_digits(const QString& s) {
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        if (!c.isDigit()) {
            out += c;
        } else if (out.isEmpty() || out.back() != QLatin1Char('#')) {
            out += QLatin1Char('#'); // a whole number becomes one #
        }
    }
    return out;
}

} // namespace

void ProblemJournal::set_context(const QString& profile, const QString& version,
                                 const QString& runId, const QString& logPath) {
    QMutexLocker lock(&mutex_);
    profile_ = profile;
    version_ = version;
    runId_   = runId;
    logPath_ = logPath;
    counts_.clear();
}

void ProblemJournal::record(const QString& level, const QString& source, const QString& message,
                            const QDateTime& nowUtc) {
    QMutexLocker lock(&mutex_);
    const int n = ++counts_[level + '|' + source + '|' + without_digits(message)];
    if (!journal_should_write(n)) {
        return;
    }
    ProblemEntry e;
    e.timeUtc = nowUtc;
    e.level   = level;
    e.kind    = "log";
    e.profile = profile_;
    e.version = version_;
    e.runId   = runId_;
    e.source  = source;
    e.message = message;
    e.repeat  = n;
    append(e);
}

void ProblemJournal::append(const ProblemEntry& e) {
    // Caller holds mutex_.
    QDir().mkpath(dir_);
    if (QFileInfo(path()).size() >= maxBytes_) {
        QFile::remove(rotated_path());
        QFile::rename(path(), rotated_path());
    }
    QFile f(path());
    if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        f.write(QJsonDocument(e.to_json()).toJson(QJsonDocument::Compact) + '\n');
    }
}

QList<ProblemEntry> ProblemJournal::read_all() const {
    QMutexLocker lock(&mutex_);
    QList<ProblemEntry> out;
    for (const QString& p : {rotated_path(), path()}) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        while (!f.atEnd()) {
            const QByteArray line = f.readLine().trimmed();
            if (line.isEmpty()) {
                continue;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (!doc.isObject()) {
                continue;
            }
            if (auto e = ProblemEntry::from_json(doc.object())) {
                out << *e;
            }
        }
    }
    return out;
}

QList<ProblemEntry> ProblemJournal::begin_run(qint64 pid, const QDateTime& nowUtc,
                                              const std::function<bool(qint64)>& isAlive,
                                              bool isAdmin) {
    QMutexLocker lock(&mutex_);
    QList<ProblemEntry> notices;
    QDir markers(marker_dir());
    QDir().mkpath(markers.absolutePath());
    QDir().mkpath(crash_dir());
    QDir().mkpath(pending_dir());

    // Oldest first: an unclean run is credited with the first dump written
    // after it started, not a later run's.
    const QFileInfoList dumps =
        QDir(crash_dir()).entryInfoList({"*.dmp"}, QDir::Files, QDir::Time | QDir::Reversed);

    for (const QFileInfo& fi : markers.entryInfoList({"*.json"}, QDir::Files)) {
        QFile f(fi.filePath());
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonObject m = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        const qint64 otherPid = m["pid"].toInteger(-1);
        // Another MOSAIC still running. A marker with this process's own id
        // but another run is a leftover from before a restart of the PC (an
        // earlier run of this process always removes its marker first).
        if (otherPid != pid && otherPid > 0 && isAlive && isAlive(otherPid)) {
            continue;
        }
        if (otherPid == pid && m["run"].toString() == runId_) {
            continue;
        }
        ProblemEntry e;
        e.timeUtc = nowUtc;
        e.level   = "ERROR";
        e.kind    = "unclean_exit";
        e.profile = m["profile"].toString();
        e.version = m["version"].toString();
        e.runId   = m["run"].toString();
        const QDateTime started =
            QDateTime::fromString(m["started_utc"].toString(), Qt::ISODateWithMs);
        QString text =
            QString(
                "MOSAIC closed unexpectedly (a crash, a power cut, or the process "
                "was ended) in the run started %1.")
                .arg(started.isValid() ? started.toLocalTime().toString("yyyy-MM-dd hh:mm:ss")
                                       : QStringLiteral("at an unknown time"));

        // A copy of that run's log, which the profile's next start would
        // otherwise push out: it is mosaic.previous.log when this start is the
        // same profile's (its log was just renamed), else still mosaic.log.
        const QString logPath = m["log"].toString();
        if (!logPath.isEmpty()) {
            const QFileInfo li(logPath);
            const QString runLog = e.profile == profile_
                                       ? li.dir().filePath(li.completeBaseName() + ".previous.log")
                                       : logPath;
            const QString copy =
                crash_dir() + "/" + safe_name(e.profile) + "_" + safe_name(e.runId) + ".log";
            QFile::remove(copy);
            if (QFile::copy(runLog, copy)) {
                text += QString(" Its log: %1.").arg(QDir::toNativeSeparators(copy));
            } else {
                text += QString(" Its log: %1.").arg(QDir::toNativeSeparators(runLog));
            }
        }
        for (const QFileInfo& d : dumps) {
            if (started.isValid() && d.lastModified().toUTC() >= started) {
                text += QString(" Crash dump: %1.").arg(QDir::toNativeSeparators(d.filePath()));
                break;
            }
        }
        e.message = text;
        append(e);
        QFile::remove(fi.filePath());

        // Told to this profile now, or kept until that profile (or an admin)
        // next starts MOSAIC.
        if (isAdmin || e.profile == profile_) {
            notices << e;
        } else {
            QSaveFile p(pending_dir() + "/" + safe_name(e.runId) + ".json");
            if (p.open(QIODevice::WriteOnly)) {
                p.write(QJsonDocument(e.to_json()).toJson(QJsonDocument::Compact));
                p.commit();
            }
        }
    }

    // Unclean exits found by another profile's start, not yet told.
    for (const QFileInfo& fi : QDir(pending_dir()).entryInfoList({"*.json"}, QDir::Files)) {
        QFile f(fi.filePath());
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        const auto e = ProblemEntry::from_json(QJsonDocument::fromJson(f.readAll()).object());
        f.close();
        if (!e) {
            QFile::remove(fi.filePath());
            continue;
        }
        if (isAdmin || e->profile == profile_) {
            notices << *e;
            QFile::remove(fi.filePath());
        }
    }

    markerPath_ = markers.filePath(QString("%1.json").arg(pid));
    QSaveFile marker(markerPath_);
    if (marker.open(QIODevice::WriteOnly)) {
        const QJsonObject m{
            {"pid", pid},          {"run", runId_},
            {"profile", profile_}, {"version", version_},
            {"log", logPath_},     {"started_utc", nowUtc.toString(Qt::ISODateWithMs)},
        };
        marker.write(QJsonDocument(m).toJson(QJsonDocument::Compact));
        marker.commit();
    }
    return notices;
}

void ProblemJournal::end_run() {
    QMutexLocker lock(&mutex_);
    if (!markerPath_.isEmpty()) {
        QFile::remove(markerPath_);
        markerPath_.clear();
    }
}

QList<ProblemEntry> problems_visible_to(const QList<ProblemEntry>& all, const QString& username,
                                        bool isAdmin) {
    if (isAdmin) {
        return all;
    }
    QList<ProblemEntry> out;
    for (const auto& e : all) {
        if (e.profile == username) {
            out << e;
        }
    }
    return out;
}

QList<ExportFile> problem_export_files(const QString& root, const QString& username, bool isAdmin,
                                       bool withDumps) {
    QList<ExportFile> out;
    auto add_logs = [&](const QString& dir, const QString& who) {
        for (const QString& name :
             {QStringLiteral("mosaic.log"), QStringLiteral("mosaic.previous.log")}) {
            const QString p = dir + "/" + name;
            if (QFileInfo(p).isFile()) {
                out.append({p, QString("logs/%1/%2").arg(who, name)});
            }
        }
    };
    // The guest's log sits at the root, every profile's in profiles/<name>/.
    if (isAdmin || username == QLatin1String("guest")) {
        add_logs(root, "guest");
    }
    const QDir profiles(root + "/profiles");
    for (const QString& who : profiles.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (isAdmin || who == username) {
            add_logs(profiles.filePath(who), who);
        }
    }
    const QDir crashes(root + "/crashes");
    // The copies of crashed runs' logs: a profile's own (named
    // <profile>_<run>.log), every one for an admin.
    for (const QFileInfo& l : crashes.entryInfoList({"*.log"}, QDir::Files, QDir::Name)) {
        if (isAdmin || l.fileName().startsWith(username + "_")) {
            out.append({l.filePath(), "crashes/" + l.fileName()});
        }
    }
    if (withDumps && isAdmin) {
        for (const QFileInfo& d : crashes.entryInfoList({"*.dmp"}, QDir::Files, QDir::Name)) {
            out.append({d.filePath(), "crashes/" + d.fileName()});
        }
    }
    return out;
}

bool process_is_alive(qint64 pid) {
    if (pid <= 0) {
        return false;
    }
#ifdef _WIN32
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        // Access denied means it exists (another user's process).
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD code     = 0;
    const bool got = ::GetExitCodeProcess(h, &code) != 0;
    ::CloseHandle(h);
    return got && code == STILL_ACTIVE;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

} // namespace mosaic
