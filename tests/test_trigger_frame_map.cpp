#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <tuple>

#include "analysis/trigger_frame_map.hpp"

using mosaic::TriggerFrameMap;

namespace {

void write_timestamps_csv(const QString& path, const QVector<std::tuple<int, int64_t>>& rows) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream ts(&f);
    ts << "frame_id,elapsed_ns,wall_ns,hw_timestamp_ns\n";
    for (const auto& [frameId, elapsedNs] : rows) {
        ts << frameId << ',' << elapsedNs << ",0,0\n";
    }
}

// New-format trigger.csv (elapsed_ns column present).
void write_trigger_csv(const QString& path, const QStringList& rows) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream ts(&f);
    ts << "elapsed_ms,elapsed_ns,wall_clock,source,label,value\n";
    for (const auto& row : rows) {
        ts << row << '\n';
    }
}

// trigger.csv including the `code` column. Deliberately kept separate from
// write_trigger_csv() above, which stays on the pre-code schema so every
// existing test doubles as backward-compatibility coverage for a session
// recorded before codes existed.
void write_trigger_csv_with_codes(const QString& path, const QStringList& rows) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream ts(&f);
    ts << "elapsed_ms,elapsed_ns,wall_clock,source,label,value,code\n";
    for (const auto& row : rows) {
        ts << row << '\n';
    }
}

// Old-format trigger.csv (no elapsed_ns column) — pre-fix schema.
void write_old_format_trigger_csv(const QString& path) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream ts(&f);
    ts << "elapsed_ms,wall_clock,source,label,value\n";
    ts << "1523.004,14:32:06.645,keyboard,Event A,0\n";
}

} // namespace

TEST(TriggerFrameMap, OldFormatWithoutElapsedNsIsInvalid) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/video"));
    write_timestamps_csv(dir.path() + "/video/timestamps_cam0.csv", {{0, 0}, {1, 40'000'000}});
    write_old_format_trigger_csv(dir.path() + "/trigger.csv");

    const auto m = TriggerFrameMap::generate(dir.path());
    EXPECT_FALSE(m.is_valid());
}

TEST(TriggerFrameMap, MissingTriggerCsvIsInvalidNotCrashing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/video"));
    write_timestamps_csv(dir.path() + "/video/timestamps_cam0.csv", {{0, 0}});

    const auto m = TriggerFrameMap::generate(dir.path());
    EXPECT_FALSE(m.is_valid());
}

TEST(TriggerFrameMap, NearestFrameCorrectnessAndTieBreak) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/video"));

    // Camera 0: frames every 40ms starting at t=0.
    write_timestamps_csv(dir.path() + "/video/timestamps_cam0.csv",
                         {{0, 0}, {1, 40'000'000}, {2, 80'000'000}, {3, 120'000'000}});

    // Rows written in ascending elapsed_ns order — matches how TriggerRecorder
    // actually appends events in real time, so rowIndex (original file order)
    // stays aligned with generate()'s internal sort-for-merge order.
    write_trigger_csv(dir.path() + "/trigger.csv",
                      {
                          // Before the first frame -> clamps to frame 0.
                          "-10.0,-10000000,14:00:00.030,parallel_port,D1_RISE,1.0",
                          // Exact match on frame 1 (t=40ms).
                          "40.0,40000000,14:00:00.000,parallel_port,D0_RISE,1.0",
                          // Exact tie between frame 1 (40ms) and frame 2 (80ms) -> t=60ms,
                          // both 20ms away; two-pointer only advances when the NEXT frame is
                          // strictly closer (not closer-or-equal), so a tie stays on the
                          // earlier frame — matches SyncManifest::generate()'s own tie-break.
                          "60.0,60000000,14:00:00.020,parallel_port,D0_FALL,0.0",
                          // After the last frame -> clamps to frame 3 (120ms).
                          "200.0,200000000,14:00:00.040,parallel_port,D1_FALL,0.0",
                      });

    const auto m = TriggerFrameMap::generate(dir.path());
    ASSERT_TRUE(m.is_valid());
    ASSERT_EQ(m.camera_count(), 1);
    ASSERT_EQ(m.trigger_count(), 4);

    EXPECT_EQ(m.row(0).frames[0].frameId, 0) << "before first frame clamps to frame 0";
    EXPECT_GT(m.row(0).frames[0].deltaMs, 0.0);

    EXPECT_EQ(m.row(1).frames[0].frameId, 1);
    EXPECT_NEAR(m.row(1).frames[0].deltaMs, 0.0, 1e-9);

    EXPECT_EQ(m.row(2).frames[0].frameId, 1) << "tie should resolve to the earlier frame";

    EXPECT_EQ(m.row(3).frames[0].frameId, 3) << "after last frame clamps to the last frame";
    EXPECT_LT(m.row(3).frames[0].deltaMs, 0.0);
}

TEST(TriggerFrameMap, VideoPositionMsMatchesEncoderPtsFormula) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/video"));

    // First frame at a non-zero elapsed_ns (as real sessions always have,
    // since elapsed_ns() is relative to app launch, not recording start).
    const int64_t firstFrameNs = 5'000'000'000LL;
    write_timestamps_csv(dir.path() + "/video/timestamps_cam0.csv",
                         {{0, firstFrameNs}, {1, firstFrameNs + 40'000'000}});

    write_trigger_csv(dir.path() + "/trigger.csv",
                      {
                          QString("40.0,%1,14:00:00.000,parallel_port,D0_RISE,1.0")
                              .arg(firstFrameNs + 40'000'000),
                      });

    const auto m = TriggerFrameMap::generate(dir.path());
    ASSERT_TRUE(m.is_valid());
    // VideoEncoder::encode_frame() sets avFrame->pts = (elapsedNs - startNs) / 1'000'000,
    // where startNs is that camera's own first frame's elapsedNs — must match exactly
    // for seek() calls to land on the right position.
    EXPECT_EQ(m.row(0).frames[0].videoPositionMs, 40);
}

TEST(TriggerFrameMap, QuotedLabelWithEmbeddedCommaParsesAsOneField) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/video"));
    write_timestamps_csv(dir.path() + "/video/timestamps_cam0.csv", {{0, 0}});

    write_trigger_csv(dir.path() + "/trigger.csv",
                      {
                          R"(0.0,0,14:00:00.000,serial,"Stimulus, S1",1.0)",
                      });

    const auto m = TriggerFrameMap::generate(dir.path());
    ASSERT_TRUE(m.is_valid());
    ASSERT_EQ(m.trigger_count(), 1);
    EXPECT_EQ(m.row(0).label, "Stimulus, S1");
    EXPECT_EQ(m.row(0).source, "serial");
}

TEST(TriggerFrameMap, SaveLoadRoundTrip) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/video"));
    write_timestamps_csv(dir.path() + "/video/timestamps_cam0.csv", {{0, 0}, {1, 40'000'000}});
    write_trigger_csv(dir.path() + "/trigger.csv",
                      {
                          "40.0,40000000,14:00:00.000,parallel_port,D0_RISE,1.0",
                      });

    auto generated = TriggerFrameMap::generate(dir.path());
    ASSERT_TRUE(generated.is_valid());
    ASSERT_TRUE(generated.save(dir.path()));

    const auto loaded = TriggerFrameMap::load(dir.path());
    ASSERT_TRUE(loaded.is_valid());
    EXPECT_EQ(loaded.camera_count(), generated.camera_count());
    EXPECT_EQ(loaded.trigger_count(), generated.trigger_count());
    EXPECT_EQ(loaded.row(0).elapsedNs, generated.row(0).elapsedNs);
    EXPECT_EQ(loaded.row(0).label, generated.row(0).label);
    EXPECT_EQ(loaded.row(0).frames[0].frameId, generated.row(0).frames[0].frameId);
    EXPECT_EQ(loaded.row(0).frames[0].videoPositionMs, generated.row(0).frames[0].videoPositionMs);
}

TEST(TriggerFrameMap, ExportCsvColumnCount) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/video"));
    write_timestamps_csv(dir.path() + "/video/timestamps_cam0.csv", {{0, 0}});
    write_timestamps_csv(dir.path() + "/video/timestamps_cam1.csv", {{0, 0}});
    write_trigger_csv(dir.path() + "/trigger.csv",
                      {
                          "0.0,0,14:00:00.000,parallel_port,D0_RISE,1.0",
                      });

    const auto m = TriggerFrameMap::generate(dir.path());
    ASSERT_TRUE(m.is_valid());
    ASSERT_EQ(m.camera_count(), 2);

    const QString csvPath = dir.path() + "/export.csv";
    ASSERT_TRUE(m.export_csv(csvPath));

    QFile f(csvPath);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QTextStream ts(&f);
    const QString header = ts.readLine();
    // 8 fixed columns
    // (trigger_row,elapsed_ns,elapsed_ms,wall_clock,source,label,code,value)
    // + 2 columns per camera (frame_id, delta_ms).
    EXPECT_EQ(header.split(',').size(), 8 + 2 * m.camera_count());
}

TEST(TriggerFrameMap, ParsesTriggerCodeAndRoundTripsItThroughJson) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/video"));
    write_timestamps_csv(dir.path() + "/video/timestamps_cam0.csv", {{0, 0}});
    write_trigger_csv_with_codes(dir.path() + "/trigger.csv",
                                 {
                                     R"(0.0,0,14:00:00.000,keyboard,"Trial onset",0.000000,7)",
                                 });

    const auto m = TriggerFrameMap::generate(dir.path());
    ASSERT_TRUE(m.is_valid());
    ASSERT_EQ(m.trigger_count(), 1);
    EXPECT_EQ(m.row(0).code, 7);

    ASSERT_TRUE(m.save(dir.path()));
    const auto loaded = TriggerFrameMap::load(dir.path());
    ASSERT_TRUE(loaded.is_valid());
    ASSERT_EQ(loaded.trigger_count(), 1);
    EXPECT_EQ(loaded.row(0).code, 7);
}

// A session recorded before codes existed must still parse, with the code
// reading 0 rather than the load failing or inventing a value.
TEST(TriggerFrameMap, TriggerCsvWithoutCodeColumnStillParsesWithCodeZero) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.path() + "/video"));
    write_timestamps_csv(dir.path() + "/video/timestamps_cam0.csv", {{0, 0}});
    write_trigger_csv(dir.path() + "/trigger.csv",
                      {
                          R"(0.0,0,14:00:00.000,keyboard,"Event A",0.000000)",
                      });

    const auto m = TriggerFrameMap::generate(dir.path());
    ASSERT_TRUE(m.is_valid());
    ASSERT_EQ(m.trigger_count(), 1);
    EXPECT_EQ(m.row(0).code, 0);
    EXPECT_EQ(m.row(0).label, "Event A");
}

TEST(TriggerFrameMap, DefaultConstructedIsInvalid) {
    const TriggerFrameMap m;
    EXPECT_FALSE(m.is_valid());
    EXPECT_EQ(m.camera_count(), 0);
    EXPECT_EQ(m.trigger_count(), 0);
}
