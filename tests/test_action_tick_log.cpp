#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "video/action_tick_log.hpp"

using mosaic::ActionTickLog;

namespace {

QList<QByteArray> lines_of(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray all        = f.readAll().replace("\r\n", "\n"); // text mode on Windows
    QList<QByteArray> out = all.split('\n');
    if (!out.isEmpty() && out.back().isEmpty()) out.removeLast();
    return out;
}

} // namespace

TEST(ActionTickLog, WritesAHeaderAndOneRowPerTick) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("action_ticks.csv");

    ActionTickLog log;
    ASSERT_TRUE(log.open(path));
    log.append(0, 1'000'000'000LL, 6);
    log.append(1, 1'040'000'000LL, 6);
    log.append(2, 1'080'000'000LL, 5);
    log.close();

    const auto lines = lines_of(path);
    ASSERT_EQ(lines.size(), 4);
    EXPECT_EQ(lines[0], "tick,elapsed_ns,fired");
    EXPECT_EQ(lines[1], "0,1000000000,6");
    EXPECT_EQ(lines[3], "2,1080000000,5");
    EXPECT_EQ(log.rows_written(), 3);
}

// What a crash leaves: rows reach the file while the log is still open.
TEST(ActionTickLog, RowsAreOnDiskBeforeClose) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("action_ticks.csv");

    ActionTickLog log;
    log.set_flush_interval_ms(0);
    ASSERT_TRUE(log.open(path));
    for (int i = 0; i < 25; ++i) log.append(i, 40'000'000LL * i, 6);
    EXPECT_EQ(lines_of(path).size(), 26);
    log.close();
}

// A log that cannot be created must not take the recording with it: append()
// and close() become no-ops.
TEST(ActionTickLog, AFailedOpenIsHarmless) {
    ActionTickLog log;
    EXPECT_FALSE(log.open("Z:/definitely/not/a/folder/action_ticks.csv"));
    EXPECT_FALSE(log.is_open());
    log.append(0, 1, 1);
    log.close();
    log.close();
    EXPECT_EQ(log.rows_written(), 0);
}
