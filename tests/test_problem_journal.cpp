#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "utils/logger.hpp"
#include "utils/problem_journal.hpp"

using namespace mosaic;

namespace {

const QDateTime kNow = QDateTime::fromString("2026-10-11T12:00:00.000Z", Qt::ISODateWithMs);

QString read(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

void write(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(data);
}

} // namespace

TEST(ProblemJournal, RecordsAWarningWithItsContextAndReadsItBack) {
    QTemporaryDir dir;
    ProblemJournal j(dir.path());
    j.set_context("payam", "0.1.0", "run-1", "x/mosaic.log");
    j.record("WARN", "video_grabber.cpp:120", "Camera 3 dropped 4 frames", kNow);

    const auto all = j.read_all();
    ASSERT_EQ(all.size(), 1);
    EXPECT_EQ(all[0].profile, "payam");
    EXPECT_EQ(all[0].version, "0.1.0");
    EXPECT_EQ(all[0].runId, "run-1");
    EXPECT_EQ(all[0].level, "WARN");
    EXPECT_EQ(all[0].kind, "log");
    EXPECT_EQ(all[0].source, "video_grabber.cpp:120");
    EXPECT_EQ(all[0].message, "Camera 3 dropped 4 frames");
    EXPECT_EQ(all[0].timeUtc, kNow);
}

TEST(ProblemJournal, ARepeatingWarningIsCountedNotRepeated) {
    EXPECT_TRUE(journal_should_write(1));
    EXPECT_TRUE(journal_should_write(3));
    EXPECT_FALSE(journal_should_write(4));
    EXPECT_TRUE(journal_should_write(10));
    EXPECT_FALSE(journal_should_write(20));
    EXPECT_TRUE(journal_should_write(1000));
    EXPECT_FALSE(journal_should_write(0));

    QTemporaryDir dir;
    ProblemJournal j(dir.path());
    j.set_context("p", "v", "r", {});
    for (int i = 0; i < 250; ++i) {
        j.record("WARN", "a.cpp:1", QString("tick %1").arg(i), kNow);
    }
    j.record("ERROR", "b.cpp:2", "other", kNow);
    const auto all = j.read_all();
    ASSERT_EQ(all.size(), 6); // occurrences 1, 2, 3, 10, 100, and the other one
    EXPECT_EQ(all[4].repeat, 100);
    EXPECT_EQ(all[4].message, "tick 99");

    // A new run counts from the start again.
    j.set_context("p", "v", "r2", {});
    j.record("WARN", "a.cpp:1", "again", kNow);
    EXPECT_EQ(j.read_all().size(), 7);
}

TEST(ProblemJournal, KeepsOneOlderFileWhenFull) {
    QTemporaryDir dir;
    ProblemJournal j(dir.path(), /*maxBytes=*/600);
    j.set_context("p", "v", "r", {});
    for (int i = 0; i < 40; ++i) {
        j.record("ERROR", QString("s.cpp:%1").arg(i), QString("message %1").arg(i), kNow);
    }
    EXPECT_TRUE(QFileInfo::exists(j.rotated_path()));
    EXPECT_LE(QFileInfo(j.path()).size(), 600 + 300);
    const auto all = j.read_all();
    ASSERT_FALSE(all.isEmpty());
    EXPECT_EQ(all.last().message, "message 39"); // newest kept
    EXPECT_LT(all.size(), 40);                   // the oldest dropped
    for (int k = 1; k < all.size(); ++k) {       // still in order
        EXPECT_LT(all[k - 1].source.mid(6).toInt(), all[k].source.mid(6).toInt());
    }
}

TEST(ProblemJournal, SkipsUnreadableLines) {
    QTemporaryDir dir;
    ProblemJournal j(dir.path());
    write(j.path(),
          "not json\n{\"time_utc\":\"2026-10-11T12:00:00.000Z\",\"level\":\"WARN\","
          "\"message\":\"ok\"}\n{\"level\":\"WARN\"}\n");
    const auto all = j.read_all();
    ASSERT_EQ(all.size(), 1);
    EXPECT_EQ(all[0].message, "ok");
}

TEST(ProblemJournal, AnUncleanExitIsToldOnceToItsOwnProfile) {
    QTemporaryDir dir;
    const QString vLog = dir.filePath("profiles/virginie/mosaic.log");
    write(vLog, "virginie's run that crashed\n");
    {
        ProblemJournal earlier(dir.path());
        earlier.set_context("virginie", "0.1.0", "run-A", vLog);
        earlier.begin_run(111, kNow.addSecs(-3600), [](qint64) { return false; });
        // ...it crashed: no end_run(). Its dump, and an older one.
        const QString dump = earlier.crash_dir() + "/mosaic_crash_A.dmp";
        const QString old  = earlier.crash_dir() + "/mosaic_crash_old.dmp";
        write(dump, "MDMP");
        write(old, "MDMP");
        QFile d(dump), o(old);
        ASSERT_TRUE(d.open(QIODevice::ReadWrite) && o.open(QIODevice::ReadWrite));
        d.setFileTime(kNow.addSecs(-1800), QFileDevice::FileModificationTime);
        o.setFileTime(kNow.addSecs(-7200), QFileDevice::FileModificationTime);
    }

    // payam (not an admin) starts next: recorded, but not told to payam.
    ProblemJournal p(dir.path());
    p.set_context("payam", "0.1.0", "run-B", {});
    EXPECT_TRUE(p.begin_run(222, kNow, [](qint64) { return false; }).isEmpty());
    const auto all = p.read_all();
    ASSERT_EQ(all.size(), 1);
    EXPECT_EQ(all[0].kind, "unclean_exit");
    EXPECT_EQ(all[0].profile, "virginie");
    EXPECT_EQ(all[0].runId, "run-A");
    EXPECT_TRUE(all[0].message.contains("mosaic_crash_A.dmp"));    // after the start
    EXPECT_FALSE(all[0].message.contains("mosaic_crash_old.dmp")); // before it
    // A copy of the crashed run's log, safe from the profile's next start.
    const QString copy = p.crash_dir() + "/virginie_run-A.log";
    EXPECT_TRUE(all[0].message.contains("virginie_run-A.log"));
    EXPECT_EQ(read(copy), "virginie's run that crashed\n");
    p.end_run();

    // virginie starts later: told now, once.
    ProblemJournal v(dir.path());
    v.set_context("virginie", "0.1.0", "run-C", vLog);
    const auto told = v.begin_run(333, kNow, [](qint64) { return false; });
    ASSERT_EQ(told.size(), 1);
    EXPECT_EQ(told[0].runId, "run-A");
    v.end_run();
    ProblemJournal v2(dir.path());
    v2.set_context("virginie", "0.1.0", "run-D", vLog);
    EXPECT_TRUE(v2.begin_run(444, kNow, [](qint64) { return false; }).isEmpty());
    EXPECT_EQ(v2.read_all().size(), 1); // recorded once
}

TEST(ProblemJournal, AnAdminIsToldOfEveryProfilesUncleanExit) {
    QTemporaryDir dir;
    ProblemJournal earlier(dir.path());
    earlier.set_context("virginie", "v", "run-A", {});
    earlier.begin_run(111, kNow, [](qint64) { return false; });

    ProblemJournal admin(dir.path());
    admin.set_context("boss", "v", "run-B", {});
    const auto told = admin.begin_run(222, kNow, [](qint64) { return false; }, /*isAdmin=*/true);
    ASSERT_EQ(told.size(), 1);
    EXPECT_EQ(told[0].profile, "virginie");
}

TEST(ProblemJournal, MessagesDifferingOnlyInNumbersCountAsOne) {
    QTemporaryDir dir;
    ProblemJournal j(dir.path());
    j.set_context("p", "v", "r", {});
    for (int i = 0; i < 5; ++i) {
        j.record("WARN", "g.cpp:9", QString("Camera %1 dropped %2 frames").arg(i).arg(i * 7), kNow);
    }
    j.record("WARN", "g.cpp:9", "Camera link down", kNow);
    EXPECT_EQ(j.read_all().size(), 4); // three of the first kind, then the other
}

TEST(ProblemJournal, AnotherRunningMosaicIsNotAnUncleanExit) {
    QTemporaryDir dir;
    ProblemJournal other(dir.path());
    other.set_context("a", "v", "run-A", {});
    other.begin_run(111, kNow, [](qint64) { return false; });

    ProblemJournal me(dir.path());
    me.set_context("b", "v", "run-B", {});
    EXPECT_TRUE(me.begin_run(222, kNow, [](qint64 pid) { return pid == 111; }).isEmpty());
}

TEST(ProblemJournal, EachProfileSeesItsOwnAndAnAdminSeesAll) {
    QList<ProblemEntry> all;
    for (const char* who : {"payam", "virginie", "payam"}) {
        ProblemEntry e;
        e.profile = who;
        all << e;
    }
    EXPECT_EQ(problems_visible_to(all, "payam", false).size(), 2);
    EXPECT_EQ(problems_visible_to(all, "nobody", false).size(), 0);
    EXPECT_EQ(problems_visible_to(all, "payam", true).size(), 3);
}

TEST(ProblemJournal, AnExportHoldsTheProfilesLogsAndDumpsOnlyForAnAdmin) {
    QTemporaryDir dir;
    const QString root = dir.path();
    write(root + "/mosaic.log", "guest");
    write(root + "/profiles/payam/mosaic.log", "p");
    write(root + "/profiles/payam/mosaic.previous.log", "p0");
    write(root + "/profiles/virginie/mosaic.log", "v");
    write(root + "/crashes/mosaic_crash_1.dmp", "MDMP");
    write(root + "/crashes/payam_run-1.log", "p crash");
    write(root + "/crashes/virginie_run-2.log", "v crash");

    auto names = [](const QList<ExportFile>& files) {
        QStringList n;
        for (const auto& f : files) n << f.name;
        return n;
    };
    EXPECT_EQ(names(problem_export_files(root, "payam", false, true)),
              (QStringList{"logs/payam/mosaic.log", "logs/payam/mosaic.previous.log",
                           "crashes/payam_run-1.log"}));
    EXPECT_EQ(names(problem_export_files(root, "guest", false, false)),
              (QStringList{"logs/guest/mosaic.log"}));
    const QStringList admin = names(problem_export_files(root, "admin", true, true));
    EXPECT_TRUE(admin.contains("logs/guest/mosaic.log"));
    EXPECT_TRUE(admin.contains("logs/virginie/mosaic.log"));
    EXPECT_TRUE(admin.contains("crashes/mosaic_crash_1.dmp"));
    EXPECT_TRUE(admin.contains("crashes/virginie_run-2.log"));
    EXPECT_FALSE(names(problem_export_files(root, "admin", true, false))
                     .contains("crashes/mosaic_crash_1.dmp"));
}

TEST(ProblemJournal, EntriesRoundTripThroughJson) {
    ProblemEntry e;
    e.timeUtc       = kNow;
    e.level         = "ERROR";
    e.kind          = "log";
    e.profile       = "p";
    e.message       = "boom";
    e.repeat        = 10;
    const auto back = ProblemEntry::from_json(e.to_json());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->repeat, 10);
    EXPECT_EQ(back->timeUtc, kNow);
    EXPECT_FALSE(e.to_json().contains("source")); // left out when empty
}

// The logger keeps the previous run's log, and hands warnings to the sink.
TEST(LoggerProblems, KeepsThePreviousLogAndFeedsWarningsToTheSink) {
    QTemporaryDir dir;
    const QString path = dir.filePath("mosaic.log");
    Logger& log        = Logger::instance();

    ASSERT_TRUE(log.open_log_file(path));
    log_info("first run");
    log.close_log_file();
    ASSERT_TRUE(log.open_log_file(path));
    log_info("second run");

    QStringList seen;
    log.set_problem_sink([&](const QString& level, const QString& source, const QString& msg) {
        seen << level + "|" + msg;
        EXPECT_TRUE(source.contains("test_problem_journal.cpp"));
    });
    log_info("not a problem");
    log_warning("a warning");
    log_error("an error");
    log.set_problem_sink({});
    log_error("after the sink is gone");
    log.close_log_file();

    EXPECT_TRUE(read(dir.filePath("mosaic.previous.log")).contains("first run"));
    EXPECT_FALSE(read(path).contains("first run"));
    EXPECT_TRUE(read(path).contains("second run"));
    EXPECT_EQ(seen, (QStringList{"WARN|a warning", "ERROR|an error"}));
}
