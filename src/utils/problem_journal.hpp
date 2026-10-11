#pragma once
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QString>
#include <functional>
#include <optional>

namespace mosaic {

/// One line of the problem journal: a warning or error MOSAIC logged, or a
/// run that ended without shutting down.
struct ProblemEntry {
    QDateTime timeUtc;
    QString level; ///< "WARN", "ERROR", "CRIT"
    QString kind;  ///< "log", or "unclean_exit" for a run that never closed
    QString profile;
    QString version;
    QString runId;
    QString source; ///< file:line of the log call (empty for unclean_exit)
    QString message;
    int repeat = 1; ///< how many times this source had fired in that run

    [[nodiscard]] QJsonObject to_json() const;
    [[nodiscard]] static std::optional<ProblemEntry> from_json(const QJsonObject& o);
};

/// Whether the n-th occurrence (1-based) of the same warning in one run is
/// written: the first three, then the 10th, 100th, 1000th... so a warning
/// that fires every frame is recorded with its count instead of filling the
/// journal.
[[nodiscard]] bool journal_should_write(int occurrence);

/// The PC's problem journal: every warning and error from every profile,
/// kept across restarts (unlike mosaic.log, which starts again at each
/// launch), in `<dir>/problems.jsonl`, one JSON object per line. When the
/// file passes `maxBytes` it becomes problems.1.jsonl (the one before is
/// dropped), so the journal stays within about twice that size.
///
/// It also notices runs that ended without shutting down: begin_run() leaves
/// a marker naming this process, end_run() removes it, and a later
/// begin_run() that finds a marker whose process is gone records an
/// "unclean_exit" entry for it (a crash, a power cut, the process killed).
///
/// Thread-safe.
class ProblemJournal {
   public:
    explicit ProblemJournal(QString dir, qint64 maxBytes = 5 * 1024 * 1024);

    [[nodiscard]] QString path() const;
    [[nodiscard]] QString rotated_path() const;
    [[nodiscard]] QString crash_dir() const; ///< `<dir>/crashes`

    /// Who and what later entries belong to.
    void set_context(const QString& profile, const QString& version, const QString& runId,
                     const QString& logPath);

    /// Records one warning or error (thinned by journal_should_write() per
    /// log call and message, numbers aside, within this run).
    void record(const QString& level, const QString& source, const QString& message,
                const QDateTime& nowUtc = QDateTime::currentDateTimeUtc());

    /// Every entry, oldest first (the rotated file first). Unreadable lines
    /// are skipped.
    [[nodiscard]] QList<ProblemEntry> read_all() const;

    /// Starts a run for process `pid`: records an unclean_exit entry for each
    /// earlier run whose process is no longer alive (per `isAlive`), with a
    /// copy of that run's log in crash_dir(), and leaves this run's marker.
    ///
    /// Returns the unclean exits to tell the person starting MOSAIC now: the
    /// current profile's (set_context()), or every one for an admin. Another
    /// profile's is kept, and returned by the first later begin_run() of
    /// that profile or of an admin, so each is told once, to its own.
    QList<ProblemEntry> begin_run(qint64 pid, const QDateTime& nowUtc,
                                  const std::function<bool(qint64)>& isAlive, bool isAdmin = false);

    /// Ends this run cleanly (removes its marker).
    void end_run();

   private:
    void append(const ProblemEntry& e);
    [[nodiscard]] QString marker_dir() const;
    [[nodiscard]] QString pending_dir() const;

    QString dir_;
    qint64 maxBytes_;
    mutable QMutex mutex_;
    QString profile_, version_, runId_, logPath_;
    QString markerPath_;
    QHash<QString, int> counts_;
};

/// The entries a profile may see: its own, or every profile's for an admin.
[[nodiscard]] QList<ProblemEntry> problems_visible_to(const QList<ProblemEntry>& all,
                                                      const QString& username, bool isAdmin);

/// One file to put in a problem export: where it is, and its name inside.
struct ExportFile {
    QString source;
    QString name;
};

/// The logs and crash dumps an export of `root` (the app-data folder) holds
/// for this profile: its mosaic.log and mosaic.previous.log and the copies of
/// its crashed runs' logs (every profile's, and the guest's, for an admin),
/// and the crash dumps when `withDumps`, for
/// an admin only: a dump holds the memory of whoever was using MOSAIC when it
/// crashed. Only files that exist are listed.
[[nodiscard]] QList<ExportFile> problem_export_files(const QString& root, const QString& username,
                                                     bool isAdmin, bool withDumps);

/// True when a process with this id is running (best effort).
[[nodiscard]] bool process_is_alive(qint64 pid);

} // namespace mosaic
