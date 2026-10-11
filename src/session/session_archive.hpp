#pragma once
#include <QDateTime>
#include <QString>
#include <atomic>
#include <functional>

#include "session/session_info.hpp"

namespace mosaic {

// ── Session archiving ──────────────────────────────────────────────────────
//
// Moves a finished session folder to another folder (an archive drive or
// share, or back from it), so the recording drive does not fill up. The
// original is only deleted once the copy is proven identical:
//
//   0. a session holding a link or junction is refused (copying one copies
//      its target, deleting the original would reach through it), and so is
//      a destination inside the session;
//   1. every file is copied into "<dest>/<name>.<computer>.partial", its
//      SHA-256 taken on the way;
//   2. every copied file is read back and its size and SHA-256 compared;
//   3. archive_manifest.json (when archiving) lists each file and its hash,
//      and the copy is renamed to its final name;
//   4. the original is renamed aside to "<name>.removing" first, which fails
//      while any of its files is open (Windows), and then the archive copy
//      is removed again so nothing is left in two places. It is then listed
//      again: if anything in it changed since step 1 (an analysis still
//      writing), it is renamed back and the copy removed;
//   5. only then is the renamed original deleted.
//
// Folders ending in .partial or .removing are left-overs of an interrupted
// move and are never listed as sessions (SessionInfo::list_all()).

struct ArchiveProgress {
    qint64 bytesDone  = 0;
    qint64 bytesTotal = 0;
    QString file; // relative path of the file being copied or checked
    bool verifying = false;
};

struct ArchiveOptions {
    /// Archiving writes archive_manifest.json into the copy. Moving a
    /// session back leaves it out (and drops the one the archive holds), so
    /// a restored session no longer reads as archived.
    bool writeManifest = true;
    /// The profile archiving it, recorded in the manifest: the owner of a
    /// session whose session_meta.json names none.
    QString archivedBy;
    /// Called from the thread doing the move.
    std::function<void(const ArchiveProgress&)> progress;
    /// Checked between chunks; set it to stop. The copy is removed and the
    /// original kept.
    const std::atomic<bool>* cancel = nullptr;
    /// Tests only: runs after the copy, before it is verified.
    std::function<void(const QString& copyDir)> afterCopy;
    /// Tests only: runs just before the (renamed) original is deleted.
    std::function<void(const QString& originalDir)> beforeDelete;
};

struct ArchiveResult {
    enum class Status { Moved, Cancelled, Failed };
    Status status = Status::Failed;
    QString message;     // what happened, for the operator
    QString destination; // the session's new folder when Moved
    qint64 bytes = 0;
    int files    = 0;
    /// Moved and verified, but part of the original could not be deleted:
    /// it is left in "<name>.removing", which is no longer listed.
    bool originalLeftOver = false;
};

/// Total size in bytes of every file under `dir` (0 if it does not exist).
[[nodiscard]] qint64 directory_size(const QString& dir);

/// Moves the session folder `sessionDir` into `destRoot` (created if
/// missing), as described above. Blocking; run it off the UI thread.
[[nodiscard]] ArchiveResult move_session_verified(const QString& sessionDir,
                                                  const QString& destRoot,
                                                  const ArchiveOptions& options = {});

/// Whether a session is old enough to be suggested for archiving: started
/// more than `days` days before `now`, finished (not being recorded) and
/// not archived already. A session with no start time is never suggested.
[[nodiscard]] bool suggest_archiving(const SessionInfo& info, int days, const QDateTime& now);

/// Why a session cannot be moved right now, or an empty string if it can.
/// `activeSessionPath` is the session this app is recording, if any.
[[nodiscard]] QString archive_blocker(const SessionInfo& info, const QString& activeSessionPath);

/// "1.2 GB", "350 MB", "12 kB".
[[nodiscard]] QString format_bytes(qint64 bytes);

} // namespace mosaic
