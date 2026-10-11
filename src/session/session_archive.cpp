#include "session/session_archive.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStorageInfo>
#include <QStringList>
#include <QSysInfo>
#include <algorithm>
#include <vector>

namespace mosaic {

namespace {

constexpr qint64 kChunk = 4 * 1024 * 1024;
// Left free on the destination beyond the session itself.
constexpr qint64 kSpareBytes = 256LL * 1024 * 1024;

#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

struct FileEntry {
    QString rel;
    qint64 size = 0;
    QDateTime modified;
    QByteArray sha256;
};

// Everything under a session folder, without following links.
struct Snapshot {
    std::vector<FileEntry> files;
    QStringList dirs;  // relative, every level
    QStringList links; // symbolic links and junctions (never followed)
};

bool snapshot_into(const QString& root, const QString& dir, Snapshot& out) {
    const QDir d(dir);
    if (!d.exists()) {
        return false;
    }
    const auto entries = d.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo& fi : entries) {
        const QString rel = QDir(root).relativeFilePath(fi.absoluteFilePath());
        if (fi.isSymLink() || fi.isJunction()) {
            out.links << rel;
        } else if (fi.isDir()) {
            out.dirs << rel;
            if (!snapshot_into(root, fi.absoluteFilePath(), out)) {
                return false;
            }
        } else {
            out.files.push_back({rel, fi.size(), fi.lastModified(), {}});
        }
    }
    return true;
}

bool take_snapshot(const QString& root, Snapshot& out) {
    out = {};
    return snapshot_into(root, root, out);
}

// The first thing that differs between two listings of the same session, or
// an empty string when they match (paths, sizes and modification times).
QString first_difference(const Snapshot& a, const Snapshot& b) {
    if (!b.links.isEmpty()) {
        return QString("%1 is a link").arg(b.links.first());
    }
    if (a.dirs != b.dirs) {
        for (const QString& x : b.dirs) {
            if (!a.dirs.contains(x)) {
                return QString("%1 appeared").arg(x);
            }
        }
        return "a folder disappeared";
    }
    if (a.files.size() != b.files.size()) {
        for (const auto& f : b.files) {
            const bool known = std::any_of(a.files.begin(), a.files.end(),
                                           [&](const FileEntry& g) { return g.rel == f.rel; });
            if (!known) {
                return QString("%1 appeared").arg(f.rel);
            }
        }
        return "a file disappeared";
    }
    for (size_t i = 0; i < a.files.size(); ++i) {
        const auto& x = a.files[i];
        const auto& y = b.files[i];
        if (x.rel != y.rel) {
            return QString("%1 appeared").arg(y.rel);
        }
        if (x.size != y.size || x.modified != y.modified) {
            return QString("%1 was written to").arg(y.rel);
        }
    }
    return {};
}

bool same_path(const QString& a, const QString& b) {
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), kPathCase) == 0;
}

// True when `inner` is `outer` or a folder inside it.
bool is_within(const QString& inner, const QString& outer) {
    const QString a = QDir::cleanPath(inner);
    const QString b = QDir::cleanPath(outer);
    return a.compare(b, kPathCase) == 0 || a.startsWith(b + "/", kPathCase);
}

// Deletes a folder tree without entering links or junctions: a link is
// removed itself, never what it points to. False if anything is left.
bool remove_tree(const QString& path) {
    bool ok            = true;
    const auto entries = QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                                  QDir::Hidden | QDir::System);
    for (const QFileInfo& fi : entries) {
        const QString p = fi.absoluteFilePath();
        if (fi.isJunction() || (fi.isSymLink() && fi.isDir())) {
            ok = QDir().rmdir(p) && ok; // the link itself
        } else if (fi.isDir() && !fi.isSymLink()) {
            ok = remove_tree(p) && ok;
        } else {
            QFile f(p);
            f.setPermissions(f.permissions() | QFileDevice::WriteOwner);
            ok = f.remove() && ok;
        }
    }
    return QDir().rmdir(path) && ok;
}

// This computer's name, safe in a folder name.
QString host_tag() {
    QString h = QSysInfo::machineHostName();
    for (QChar& c : h) {
        if (!c.isLetterOrNumber() && c != '-' && c != '_') {
            c = '_';
        }
    }
    return h.isEmpty() ? QStringLiteral("pc") : h;
}

bool cancelled(const ArchiveOptions& o) { return o.cancel && o.cancel->load(); }

void report(const ArchiveOptions& o, qint64 done, qint64 total, const QString& file,
            bool verifying) {
    if (o.progress) {
        o.progress({done, total, file, verifying});
    }
}

ArchiveResult failed(const QString& message) {
    ArchiveResult r;
    r.status  = ArchiveResult::Status::Failed;
    r.message = message;
    return r;
}

// Copies one file, hashing what it reads. False on any read/write error or
// a cancel; `hash` is the SHA-256 of the source.
bool copy_hashed(const QString& from, const QString& to, const ArchiveOptions& o, qint64& done,
                 qint64 total, const QString& rel, QByteArray& hash, QString& error) {
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly)) {
        error = QString("Could not read %1: %2").arg(rel, in.errorString());
        return false;
    }
    QFile out(to);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QString("Could not write %1 in the archive: %2").arg(rel, out.errorString());
        return false;
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    while (!in.atEnd()) {
        if (cancelled(o)) {
            return false;
        }
        const QByteArray chunk = in.read(kChunk);
        if (chunk.isEmpty() && in.error() != QFileDevice::NoError) {
            error = QString("Could not read %1: %2").arg(rel, in.errorString());
            return false;
        }
        h.addData(chunk);
        if (out.write(chunk) != chunk.size()) {
            error = QString("Could not write %1 in the archive: %2").arg(rel, out.errorString());
            return false;
        }
        done += chunk.size();
        report(o, done, total, rel, false);
    }
    if (!out.flush()) {
        error = QString("Could not write %1 in the archive: %2").arg(rel, out.errorString());
        return false;
    }
    out.close();
    // Keep the original's time stamps, which file managers and backups use.
    if (out.open(QIODevice::ReadWrite)) {
        out.setFileTime(QFileInfo(from).lastModified(), QFileDevice::FileModificationTime);
        out.close();
    }
    hash = h.result();
    return true;
}

bool hash_file(const QString& path, const ArchiveOptions& o, qint64& done, qint64 total,
               const QString& rel, QByteArray& hash) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    while (!f.atEnd()) {
        if (cancelled(o)) {
            return false;
        }
        const QByteArray chunk = f.read(kChunk);
        if (chunk.isEmpty() && f.error() != QFileDevice::NoError) {
            return false;
        }
        h.addData(chunk);
        done += chunk.size();
        report(o, done, total, rel, true);
    }
    hash = h.result();
    return true;
}

} // namespace

qint64 directory_size(const QString& dir) {
    Snapshot snap;
    if (!take_snapshot(dir, snap)) {
        return 0;
    }
    qint64 total = 0;
    for (const auto& f : snap.files) {
        total += f.size;
    }
    return total;
}

ArchiveResult move_session_verified(const QString& sessionDir, const QString& destRoot,
                                    const ArchiveOptions& options) {
    const QFileInfo srcInfo(sessionDir);
    const QString src = QDir::cleanPath(srcInfo.absoluteFilePath());
    if (!srcInfo.isDir() || !QFile::exists(src + "/session_meta.json")) {
        return failed(QString("%1 is not a session folder.").arg(sessionDir));
    }
    if (destRoot.trimmed().isEmpty()) {
        return failed("No destination folder is set.");
    }
    const QString root = QDir::cleanPath(QFileInfo(destRoot).absoluteFilePath());
    if (is_within(root, src)) {
        return failed("The destination folder is inside the session; nothing was moved.");
    }
    const QString name = srcInfo.fileName();
    const QString dest = root + "/" + name;
    if (same_path(QFileInfo(src).absolutePath(), root)) {
        return failed("The session is already in that folder.");
    }
    if (QFileInfo::exists(dest)) {
        return failed(
            QString("%1 already holds a folder named %2; nothing was moved.").arg(root, name));
    }

    // What the session holds. Links and junctions are refused: copying one
    // copies what it points to, and deleting the original would then reach
    // through it into a folder outside the session.
    Snapshot before;
    if (!take_snapshot(src, before)) {
        return failed(QString("Could not list the files of %1.").arg(name));
    }
    if (!before.links.isEmpty()) {
        return failed(QString("The session contains a link or junction (%1); move it by hand. "
                              "Nothing was moved.")
                          .arg(before.links.first()));
    }
    // The manifest of an earlier archiving is not carried along: an archived
    // copy gets a fresh one, a restored session none.
    std::vector<FileEntry> files;
    qint64 total = 0;
    for (const auto& f : before.files) {
        if (f.rel != QLatin1String(kArchiveManifestFile)) {
            files.push_back(f);
            total += f.size;
        }
    }

    if (!QDir().mkpath(root)) {
        return failed(QString("Could not create %1.").arg(root));
    }
    const QStorageInfo storage(root);
    if (storage.isValid() && storage.bytesAvailable() < total + kSpareBytes) {
        return failed(QString("Not enough space in %1: the session needs %2, %3 is free.")
                          .arg(root, format_bytes(total), format_bytes(storage.bytesAvailable())));
    }

    // Named after this computer, so a move started from another PC sharing
    // the archive is never mistaken for a left-over of this one.
    const QString partial = dest + "." + host_tag() + ".partial";
    if (QFileInfo::exists(partial)) {
        QDir(partial).removeRecursively(); // an interrupted earlier attempt from here
    }
    if (!QDir().mkpath(partial)) {
        return failed(QString("Could not create %1.").arg(partial));
    }
    auto abandon = [&](ArchiveResult r) {
        QDir(partial).removeRecursively();
        return r;
    };
    auto stopped = [&] {
        ArchiveResult r;
        r.status  = ArchiveResult::Status::Cancelled;
        r.message = "Cancelled; the session was not moved.";
        return abandon(r);
    };

    // 1. Copy, hashing the source on the way; the folders too, empty ones
    // included.
    for (const QString& dir : before.dirs) {
        if (!QDir().mkpath(partial + "/" + dir)) {
            return abandon(failed(QString("Could not create %1 in the archive.").arg(dir)));
        }
    }
    qint64 done = 0;
    for (auto& f : files) {
        const QString to = partial + "/" + f.rel;
        if (!QDir().mkpath(QFileInfo(to).absolutePath())) {
            return abandon(
                failed(QString("Could not create a folder for %1 in the archive.").arg(f.rel)));
        }
        QString error;
        if (!copy_hashed(src + "/" + f.rel, to, options, done, total, f.rel, f.sha256, error)) {
            if (cancelled(options)) {
                return stopped();
            }
            return abandon(failed(error + " The session was not moved."));
        }
    }
    if (options.afterCopy) {
        options.afterCopy(partial);
    }

    // 2. Read every copied file back and compare.
    qint64 checked = 0;
    for (const auto& f : files) {
        const QString copy = partial + "/" + f.rel;
        QByteArray hash;
        if (QFileInfo(copy).size() != f.size ||
            !hash_file(copy, options, checked, total, f.rel, hash) || hash != f.sha256) {
            if (cancelled(options)) {
                return stopped();
            }
            return abandon(failed(QString("The archived copy of %1 does not match the "
                                          "original; the session was not moved.")
                                      .arg(f.rel)));
        }
    }

    // 3. Manifest, then the final name.
    if (options.writeManifest) {
        QJsonArray list;
        for (const auto& f : files) {
            list.append(QJsonObject{{"path", f.rel},
                                    {"size", f.size},
                                    {"sha256", QString::fromLatin1(f.sha256.toHex())}});
        }
        const QJsonObject manifest{
            {"schema", "mosaic-archive-v1"},
            {"archived_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"archived_from", src},
            {"archived_by", options.archivedBy},
            {"computer", QSysInfo::machineHostName()},
            {"total_bytes", total},
            {"files", list},
        };
        const QByteArray json = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
        QFile mf(partial + "/" + QLatin1String(kArchiveManifestFile));
        const bool written =
            mf.open(QIODevice::WriteOnly) && mf.write(json) == json.size() && mf.flush();
        mf.close();
        if (!written || mf.error() != QFileDevice::NoError) {
            return abandon(
                failed("Could not write the archive manifest; the session was not "
                       "moved."));
        }
    }
    if (!QDir().rename(partial, dest)) {
        return abandon(failed(QString("Could not rename the copy to %1; the session was not "
                                      "moved.")
                                  .arg(dest)));
    }

    // 4. Take the original out of the list in one step. Renaming fails while
    // a file in it is open (a video playing, an analysis writing); then the
    // copy goes, so the session is not left in two places.
    QString removing = src + ".removing";
    for (int n = 2; QFileInfo::exists(removing); ++n) {
        removing = QString("%1.%2.removing").arg(src).arg(n);
    }
    if (!QDir().rename(src, removing)) {
        QDir(dest).removeRecursively();
        return failed(
            "Some of the session's files are in use (a video playing, or an analysis running "
            "on it); close them and try again. Nothing was moved.");
    }

    // Anything written into the session since it was listed (an analysis
    // still running, another program) is not in the copy. Now that the folder
    // has a new name nothing can add to it by its old path, so compare once
    // more; on any difference put everything back.
    Snapshot after;
    QString changed;
    if (!take_snapshot(removing, after)) {
        changed = "the session could not be listed again";
    } else {
        changed = first_difference(before, after);
    }
    if (!changed.isEmpty()) {
        if (QDir().rename(removing, src)) {
            QDir(dest).removeRecursively();
            return failed(QString("The session changed while it was being moved (%1). Nothing "
                                  "was moved; try again once nothing is writing to it.")
                              .arg(changed));
        }
        // Both kept: the original under its temporary name, and the copy
        // (which lacks the change) hidden as a .partial so it is not listed as
        // a normal archived session someone could move back.
        QString hidden = dest + "." + host_tag() + ".unverified.partial";
        if (!QDir().rename(dest, hidden)) {
            hidden = dest;
        }
        return failed(QString("The session changed while it was being moved (%1), and its "
                              "folder could not be renamed back. Nothing was deleted: the "
                              "original is %2, a copy without the change is %3. Rename the "
                              "original back by hand and delete the copy.")
                          .arg(changed, QDir::toNativeSeparators(removing),
                               QDir::toNativeSeparators(hidden)));
    }

    // 5. Delete the original, never following a link (one could have been
    // made since the second listing). Not caught: a same-size rewrite within
    // the file system's time stamp resolution (2 s on FAT32) between the
    // listings; NTFS and network shares keep much finer times.
    ArchiveResult r;
    r.status      = ArchiveResult::Status::Moved;
    r.destination = dest;
    r.bytes       = total;
    r.files       = static_cast<int>(files.size());
    if (options.beforeDelete) {
        options.beforeDelete(removing);
    }
    if (remove_tree(removing)) {
        r.message = QString("Moved %1 to %2 (%3 files, %4, verified).")
                        .arg(name, root)
                        .arg(r.files)
                        .arg(format_bytes(total));
    } else {
        r.originalLeftOver = true;
        r.message          = QString(
                        "Moved and verified, but part of the original could not be deleted "
                                 "(a file still open?). Delete %1 by hand.")
                        .arg(QDir::toNativeSeparators(removing));
    }
    return r;
}

bool suggest_archiving(const SessionInfo& info, int days, const QDateTime& now) {
    if (!info.startUtc.isValid() || info.recordingNow || info.archived) {
        return false;
    }
    return info.startUtc.addDays(days) < now;
}

QString archive_blocker(const SessionInfo& info, const QString& activeSessionPath) {
    if (info.recordingNow) {
        return "It is being recorded.";
    }
    if (!activeSessionPath.isEmpty() &&
        QDir::cleanPath(QFileInfo(activeSessionPath).absoluteFilePath()) ==
            QDir::cleanPath(QFileInfo(info.path).absoluteFilePath())) {
        return "It is being recorded.";
    }
    return {};
}

QString format_bytes(qint64 bytes) {
    const double b = static_cast<double>(bytes);
    if (b >= 1e12) {
        return QString("%1 TB").arg(b / 1e12, 0, 'f', 2);
    }
    if (b >= 1e9) {
        return QString("%1 GB").arg(b / 1e9, 0, 'f', 1);
    }
    if (b >= 1e6) {
        return QString("%1 MB").arg(b / 1e6, 0, 'f', 0);
    }
    if (b >= 1e3) {
        return QString("%1 kB").arg(b / 1e3, 0, 'f', 0);
    }
    return QString("%1 B").arg(bytes);
}

} // namespace mosaic
