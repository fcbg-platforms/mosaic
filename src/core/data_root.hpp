#pragma once
#include <QList>
#include <QString>

namespace mosaic {

// ── Where MOSAIC's data lives ──────────────────────────────────────────────
//
// Recording folders are saved as relative paths ("./recordings/<profile>"),
// which resolve against the process's working folder. That used to be
// wherever MOSAIC happened to be started from: started from the build
// output, a profile's sessions landed inside build\...\bin\Release, where a
// clean rebuild deletes them; started from the checkout, they landed in its
// recordings\ folder. MOSAIC now makes data_root() its working folder at
// startup, so a relative path means the same folder however it is started.

/// The source checkout `exeDir` belongs to: the nearest folder at or above
/// it (up to 6 levels) holding analysis/pyproject.toml. Empty if none.
[[nodiscard]] QString find_checkout_root(const QString& exeDir);

/// MOSAIC's data folder: the checkout it runs from, else
/// Documents/MOSAIC (an installed copy).
[[nodiscard]] QString data_root();

/// One session to move from the old place to the data folder.
struct SessionMove {
    QString session;    ///< the session folder now
    QString destParent; ///< the folder it goes into
};

/// Sessions left in `oldRecordings` (recordings\ beside the program, the
/// old place) that belong in `newRecordings` (the data folder's), keeping
/// each one's sub-folder (recordings\virginie\X goes to ...\virginie\X).
/// An admin moves every profile's; anyone else only those in `ownSub` (the
/// profile's own sub-folder, e.g. "virginie"). Nothing when both are the
/// same folder.
[[nodiscard]] QList<SessionMove> stranded_sessions(const QString& oldRecordings,
                                                   const QString& newRecordings,
                                                   const QString& ownSub, bool isAdmin);

} // namespace mosaic
