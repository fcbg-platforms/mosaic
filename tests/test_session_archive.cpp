#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSysInfo>
#include <QTemporaryDir>
#include <atomic>

#include "core/settings.hpp"
#include "session/session_archive.hpp"

using namespace mosaic;

namespace {

void write_file(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QByteArray read_file(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A small session: meta, a "video" bigger than one copy chunk, nested
// analysis output and an empty folder.
QString make_session(const QString& root, const QString& name = "sub-01_ses-1_run-01") {
    const QString dir = root + "/" + name;
    write_file(dir + "/session_meta.json",
               R"({"recorded_by":"payam","session_start_utc":"2026-09-01T10:00:00.000Z",)"
               R"("session_end":{"duration_ms":1000}})");
    QByteArray video(5 * 1024 * 1024 + 123, '\0');
    for (int i = 0; i < video.size(); ++i) {
        video[i] = static_cast<char>((i * 31) & 0xff);
    }
    write_file(dir + "/video/video_0.mp4", video);
    write_file(dir + "/pose/video_0.pose.json", R"({"frames":[]})");
    QDir().mkpath(dir + "/empty_folder");
    return dir;
}

} // namespace

TEST(SessionArchive, MovesVerifiesAndWritesAManifest) {
    QTemporaryDir rec, arc;
    const QString src      = make_session(rec.path());
    const QByteArray video = read_file(src + "/video/video_0.mp4");
    std::vector<ArchiveProgress> seen;
    ArchiveOptions opt;
    opt.progress = [&](const ArchiveProgress& p) { seen.push_back(p); };

    const auto r = move_session_verified(src, arc.path(), opt);

    ASSERT_EQ(r.status, ArchiveResult::Status::Moved) << r.message.toStdString();
    EXPECT_FALSE(QFileInfo::exists(src));
    EXPECT_FALSE(QFileInfo::exists(src + ".removing"));
    const QString dest = arc.path() + "/sub-01_ses-1_run-01";
    EXPECT_EQ(QDir::cleanPath(r.destination), QDir::cleanPath(dest));
    EXPECT_EQ(read_file(dest + "/video/video_0.mp4"), video);
    EXPECT_TRUE(QFileInfo(dest + "/empty_folder").isDir());
    EXPECT_EQ(r.files, 3);

    const auto manifest =
        QJsonDocument::fromJson(read_file(dest + "/" + kArchiveManifestFile)).object();
    EXPECT_EQ(manifest["schema"].toString(), "mosaic-archive-v1");
    EXPECT_EQ(manifest["files"].toArray().size(), 3);
    EXPECT_EQ(manifest["total_bytes"].toInteger(), r.bytes);
    EXPECT_TRUE(SessionInfo::load(dest).archived);

    // Progress covers the copy and the read-back, ending at the total.
    ASSERT_FALSE(seen.empty());
    EXPECT_FALSE(seen.front().verifying);
    EXPECT_TRUE(seen.back().verifying);
    EXPECT_EQ(seen.back().bytesDone, r.bytes);
}

TEST(SessionArchive, MovingBackDropsTheManifest) {
    QTemporaryDir rec, arc;
    const QString src = make_session(rec.path());
    ASSERT_EQ(move_session_verified(src, arc.path()).status, ArchiveResult::Status::Moved);

    ArchiveOptions back;
    back.writeManifest = false;
    const auto r = move_session_verified(arc.path() + "/sub-01_ses-1_run-01", rec.path(), back);

    ASSERT_EQ(r.status, ArchiveResult::Status::Moved) << r.message.toStdString();
    EXPECT_TRUE(QFileInfo::exists(src + "/video/video_0.mp4"));
    EXPECT_FALSE(QFileInfo::exists(src + "/" + kArchiveManifestFile));
    EXPECT_FALSE(SessionInfo::load(src).archived);
}

TEST(SessionArchive, AMismatchedCopyKeepsTheOriginalAndLeavesNoCopy) {
    QTemporaryDir rec, arc;
    const QString src = make_session(rec.path());
    ArchiveOptions opt;
    opt.afterCopy = [](const QString& copy) {
        QFile f(copy + "/video/video_0.mp4");
        ASSERT_TRUE(f.open(QIODevice::ReadWrite));
        f.seek(1000);
        f.write("X"); // same size, different content
    };

    const auto r = move_session_verified(src, arc.path(), opt);

    EXPECT_EQ(r.status, ArchiveResult::Status::Failed);
    EXPECT_TRUE(r.message.contains("video/video_0.mp4"));
    EXPECT_TRUE(QFileInfo::exists(src + "/video/video_0.mp4"));
    EXPECT_TRUE(QDir(arc.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

TEST(SessionArchive, CancelKeepsTheOriginalAndLeavesNoCopy) {
    QTemporaryDir rec, arc;
    const QString src = make_session(rec.path());
    std::atomic<bool> cancel{false};
    ArchiveOptions opt;
    opt.cancel   = &cancel;
    opt.progress = [&](const ArchiveProgress&) { cancel = true; };

    const auto r = move_session_verified(src, arc.path(), opt);

    EXPECT_EQ(r.status, ArchiveResult::Status::Cancelled);
    EXPECT_TRUE(QFileInfo::exists(src + "/video/video_0.mp4"));
    EXPECT_TRUE(QDir(arc.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

TEST(SessionArchive, NeverOverwritesAFolderAlreadyInTheArchive) {
    QTemporaryDir rec, arc;
    const QString src = make_session(rec.path());
    write_file(arc.path() + "/sub-01_ses-1_run-01/keep.txt", "older copy");

    const auto r = move_session_verified(src, arc.path());

    EXPECT_EQ(r.status, ArchiveResult::Status::Failed);
    EXPECT_TRUE(QFileInfo::exists(src + "/session_meta.json"));
    EXPECT_EQ(read_file(arc.path() + "/sub-01_ses-1_run-01/keep.txt"), "older copy");
}

TEST(SessionArchive, RefusesAFolderThatIsNotASession) {
    QTemporaryDir rec, arc;
    write_file(rec.path() + "/notes/readme.txt", "hello");
    const auto r = move_session_verified(rec.path() + "/notes", arc.path());
    EXPECT_EQ(r.status, ArchiveResult::Status::Failed);
    EXPECT_TRUE(QFileInfo::exists(rec.path() + "/notes/readme.txt"));
}

TEST(SessionArchive, ALeftOverCopyFromThisComputerIsReplaced) {
    QTemporaryDir rec, arc;
    const QString src = make_session(rec.path());
    QString host      = QSysInfo::machineHostName();
    for (QChar& c : host) {
        if (!c.isLetterOrNumber() && c != '-' && c != '_') {
            c = '_';
        }
    }
    const QString mine  = arc.path() + "/sub-01_ses-1_run-01." + host + ".partial";
    const QString other = arc.path() + "/sub-01_ses-1_run-01.OTHER-PC.partial";
    write_file(mine + "/video/video_0.mp4", "truncated");
    write_file(other + "/video/video_0.mp4", "another PC's copy in progress");

    const auto r = move_session_verified(src, arc.path());

    ASSERT_EQ(r.status, ArchiveResult::Status::Moved) << r.message.toStdString();
    EXPECT_FALSE(QFileInfo::exists(mine));
    EXPECT_TRUE(QFileInfo::exists(other + "/video/video_0.mp4")); // not ours to delete
}

TEST(SessionArchive, AFileWrittenDuringTheMoveUndoesIt) {
    // An analysis still writing into the session: its new file is not in the
    // copy, so the original must not be deleted.
    QTemporaryDir rec, arc;
    const QString src = make_session(rec.path());
    ArchiveOptions opt;
    opt.afterCopy = [&](const QString&) { write_file(src + "/pose/late.json", "{}"); };

    const auto r = move_session_verified(src, arc.path(), opt);

    EXPECT_EQ(r.status, ArchiveResult::Status::Failed);
    EXPECT_TRUE(r.message.contains("late.json")) << r.message.toStdString();
    EXPECT_TRUE(QFileInfo::exists(src + "/pose/late.json"));
    EXPECT_TRUE(QFileInfo::exists(src + "/video/video_0.mp4"));
    EXPECT_TRUE(QDir(arc.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

TEST(SessionArchive, AFileChangedDuringTheMoveUndoesIt) {
    QTemporaryDir rec, arc;
    const QString src = make_session(rec.path());
    ArchiveOptions opt;
    opt.afterCopy = [&](const QString&) {
        write_file(src + "/pose/video_0.pose.json", R"({"frames":[1,2,3]})");
    };

    const auto r = move_session_verified(src, arc.path(), opt);

    EXPECT_EQ(r.status, ArchiveResult::Status::Failed);
    EXPECT_EQ(read_file(src + "/pose/video_0.pose.json"), R"({"frames":[1,2,3]})");
    EXPECT_FALSE(QFileInfo::exists(arc.path() + "/sub-01_ses-1_run-01"));
}

TEST(SessionArchive, RefusesADestinationInsideTheSession) {
    QTemporaryDir rec;
    const QString src = make_session(rec.path());
    const auto r      = move_session_verified(src, src + "/archive");
    EXPECT_EQ(r.status, ArchiveResult::Status::Failed);
    EXPECT_FALSE(QFileInfo::exists(src + "/archive"));
    EXPECT_TRUE(QFileInfo::exists(src + "/video/video_0.mp4"));
}

#ifdef Q_OS_WIN
// Windows will not rename a folder while a file in it is open: the move is
// undone so the session is not left in both places.
// A junction inside a session would be followed: its target copied, then
// emptied when the original is deleted. Such a session is refused.
TEST(SessionArchive, ASessionWithAJunctionIsRefusedAndItsTargetKept) {
    QTemporaryDir rec, arc, outside;
    const QString src = make_session(rec.path());
    write_file(outside.path() + "/precious.txt", "keep me");
    QProcess mk;
    mk.start("cmd", {"/c", "mklink", "/J", QDir::toNativeSeparators(src + "/link"),
                     QDir::toNativeSeparators(outside.path())});
    ASSERT_TRUE(mk.waitForFinished(10000));
    ASSERT_TRUE(QFileInfo(src + "/link").isJunction());

    const auto r = move_session_verified(src, arc.path());

    EXPECT_EQ(r.status, ArchiveResult::Status::Failed);
    EXPECT_TRUE(r.message.contains("link")) << r.message.toStdString();
    EXPECT_EQ(read_file(outside.path() + "/precious.txt"), "keep me");
    EXPECT_TRUE(QFileInfo::exists(src + "/session_meta.json"));
    EXPECT_TRUE(QDir(arc.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    QDir().rmdir(src + "/link"); // the junction itself, not its target
}

// A junction made after the second listing, just before the original is
// deleted: the delete removes the junction itself, never what it points to.
TEST(SessionArchive, TheFinalDeleteNeverFollowsAJunction) {
    QTemporaryDir rec, arc, outside;
    const QString src = make_session(rec.path());
    write_file(outside.path() + "/precious.txt", "keep me");
    ArchiveOptions opt;
    opt.beforeDelete = [&](const QString& original) {
        QProcess mk;
        mk.start("cmd", {"/c", "mklink", "/J", QDir::toNativeSeparators(original + "/link"),
                         QDir::toNativeSeparators(outside.path())});
        ASSERT_TRUE(mk.waitForFinished(10000));
        ASSERT_TRUE(QFileInfo(original + "/link").isJunction());
    };

    const auto r = move_session_verified(src, arc.path(), opt);

    ASSERT_EQ(r.status, ArchiveResult::Status::Moved) << r.message.toStdString();
    EXPECT_FALSE(r.originalLeftOver);
    EXPECT_EQ(read_file(outside.path() + "/precious.txt"), "keep me");
    EXPECT_FALSE(QFileInfo::exists(src + ".removing"));
}

TEST(SessionArchive, AnOpenFileUndoesTheMove) {
    QTemporaryDir rec, arc;
    const QString src = make_session(rec.path());
    QFile open(src + "/video/video_0.mp4");
    ASSERT_TRUE(open.open(QIODevice::ReadOnly));

    const auto r = move_session_verified(src, arc.path());

    EXPECT_EQ(r.status, ArchiveResult::Status::Failed);
    EXPECT_TRUE(r.message.contains("in use"));
    EXPECT_TRUE(QFileInfo::exists(src + "/session_meta.json"));
    EXPECT_TRUE(QDir(arc.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}
#endif

TEST(SessionArchive, LeftOverFoldersAreNotListedAsSessions) {
    QTemporaryDir rec;
    make_session(rec.path(), "a");
    make_session(rec.path(), "b.partial");
    make_session(rec.path(), "c.removing");
    const auto all = SessionInfo::list_all(rec.path());
    ASSERT_EQ(all.size(), 1);
    EXPECT_EQ(all[0].name, "a");
}

TEST(SessionArchive, SuggestsOnlyOldFinishedUnarchivedSessions) {
    const QDateTime now = QDateTime::fromString("2026-10-11T12:00:00Z", Qt::ISODate);
    SessionInfo s;
    s.startUtc = now.addDays(-40);
    EXPECT_TRUE(suggest_archiving(s, 30, now));
    EXPECT_FALSE(suggest_archiving(s, 60, now));
    s.recordingNow = true;
    EXPECT_FALSE(suggest_archiving(s, 30, now));
    s.recordingNow = false;
    s.archived     = true;
    EXPECT_FALSE(suggest_archiving(s, 30, now));
    EXPECT_FALSE(suggest_archiving(SessionInfo{}, 30, now)); // no start time
}

TEST(SessionArchive, TheSessionBeingRecordedCannotBeMoved) {
    SessionInfo s;
    s.path = "D:/rec/session_a";
    EXPECT_TRUE(archive_blocker(s, {}).isEmpty());
    EXPECT_FALSE(archive_blocker(s, "D:/rec/session_a").isEmpty());
    EXPECT_TRUE(archive_blocker(s, "D:/rec/session_b").isEmpty());
    s.recordingNow = true;
    EXPECT_FALSE(archive_blocker(s, {}).isEmpty());
}

TEST(SessionArchive, FormatsSizes) {
    EXPECT_EQ(format_bytes(512), "512 B");
    EXPECT_EQ(format_bytes(12'000), "12 kB");
    EXPECT_EQ(format_bytes(350'000'000), "350 MB");
    EXPECT_EQ(format_bytes(1'234'000'000), "1.2 GB");
}

TEST(SessionArchive, ArchiveSettingsRoundTripAndAreKeptInRange) {
    RecordSettings rec;
    rec.archiveDirectory = "N:/mosaic_archive/payam";
    rec.archiveAfterDays = 45;
    const auto loaded    = RecordSettings::from_json(rec.to_json());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->archiveDirectory, "N:/mosaic_archive/payam");
    EXPECT_EQ(loaded->archiveAfterDays, 45);

    const auto days = [](int v) {
        return RecordSettings::from_json(QJsonObject{{"archive_after_days", v}})->archiveAfterDays;
    };
    EXPECT_EQ(days(0), 1);
    EXPECT_EQ(days(100000), RecordSettings::kMaxArchiveAfterDays);
    EXPECT_TRUE(RecordSettings::from_json(QJsonObject{})->archiveDirectory.isEmpty());
}
