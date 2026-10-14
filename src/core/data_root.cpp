#include "core/data_root.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace mosaic {

namespace {

// The real folder (links and junctions resolved) when it exists.
QString real_path(const QString& p) {
    const QFileInfo fi(p);
    const QString canonical = fi.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? fi.absoluteFilePath() : canonical);
}

bool same_folder(const QString& a, const QString& b) {
    const QString x = real_path(a);
    const QString y = real_path(b);
#ifdef Q_OS_WIN
    return x.compare(y, Qt::CaseInsensitive) == 0;
#else
    return x == y;
#endif
}

bool is_session(const QString& dir) { return QFileInfo::exists(dir + "/session_meta.json"); }

bool is_leftover(const QString& name) {
    return name.endsWith(".partial") || name.endsWith(".removing");
}

} // namespace

QString find_checkout_root(const QString& exeDir) {
    QDir d(exeDir);
    for (int level = 0; level <= 6; ++level) {
        if (QFileInfo::exists(d.filePath("analysis/pyproject.toml"))) {
            return QDir::cleanPath(d.absolutePath());
        }
        if (!d.cdUp()) {
            break;
        }
    }
    return {};
}

QString data_root() {
    const QString checkout = find_checkout_root(QCoreApplication::applicationDirPath());
    if (!checkout.isEmpty()) {
        return checkout;
    }
    return QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                           "/MOSAIC");
}

QList<SessionMove> stranded_sessions(const QString& oldRecordings, const QString& newRecordings,
                                     const QString& ownSub, bool isAdmin) {
    QList<SessionMove> out;
    if (oldRecordings.isEmpty() || same_folder(oldRecordings, newRecordings)) {
        return out;
    }
    const QDir old(oldRecordings);
    if (!old.exists()) {
        return out;
    }
    for (const QFileInfo& sub : old.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (is_leftover(sub.fileName())) {
            continue;
        }
        if (is_session(sub.filePath())) {
            // A session loose in recordings\ itself (the old shared layout).
            if (isAdmin) {
                out.append({sub.filePath(), newRecordings});
            }
            continue;
        }
        const bool own = sub.fileName().compare(ownSub, Qt::CaseInsensitive) == 0;
        if (!isAdmin && !own) {
            continue;
        }
        const QDir profileDir(sub.filePath());
        for (const QFileInfo& s :
             profileDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (!is_leftover(s.fileName()) && is_session(s.filePath())) {
                out.append({s.filePath(), newRecordings + "/" + sub.fileName()});
            }
        }
    }
    return out;
}

} // namespace mosaic
