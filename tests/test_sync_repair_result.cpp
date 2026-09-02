#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "analysis/sync_repair_result.hpp"

using mosaic::SyncRepairResult;

namespace {

// Matches the exact schema written by analysis/run_sync_repair.py's
// process_session(). 2 successfully-repaired cameras + 1 skipped camera
// (no video/timestamps file) + 1 camera with a truncated-source-video note.
const char* kFixtureJson = R"JSON(
{
  "schema": "mosaic-sync-repair-v1",
  "master_fps": 12.7476,
  "total_ticks": 23431,
  "duration_ms": 1837452,
  "generated_at_utc": "2026-08-27T10:00:00Z",
  "cameras": [
    {"index": 0, "source_video": "video/video_0.mp4", "repaired_video": "synced/video_0.mp4",
     "source_frames_captured": 23470, "output_frame_count": 23431,
     "duplicated_frame_count": 12, "skipped": false, "skip_reason": "", "note": ""},
    {"index": 1, "source_video": "video/video_1.mp4", "repaired_video": "synced/video_1.mp4",
     "source_frames_captured": 23469, "output_frame_count": 23431,
     "duplicated_frame_count": 15, "skipped": false, "skip_reason": "",
     "note": "source video ended early (only 23400 of an expected 23469 frame(s) could be decoded) - remaining ticks were filled by duplicating the last successfully-decoded frame"},
    {"index": 5, "source_video": null, "repaired_video": null,
     "source_frames_captured": 0, "output_frame_count": 0,
     "duplicated_frame_count": 0, "skipped": true,
     "skip_reason": "no video_N.mp4 found; no timestamps_camN.csv found", "note": ""}
  ]
}
)JSON";

QString write_fixture(const QString& dirPath) {
    const QString path = dirPath + "/sync_repair.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(kFixtureJson);
    return path;
}

} // namespace

TEST(SyncRepairResult, LoadsValidFileWithMixedCameraOutcomes) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = SyncRepairResult::load(write_fixture(dir.path()));

    ASSERT_TRUE(result.is_valid());
    EXPECT_DOUBLE_EQ(result.master_fps(), 12.7476);
    EXPECT_EQ(result.total_ticks(), 23431);
    EXPECT_EQ(result.duration_ms(), 1837452);

    ASSERT_EQ(result.cameras().size(), 3);

    const auto& cam0 = result.cameras()[0];
    EXPECT_EQ(cam0.index, 0);
    EXPECT_EQ(cam0.sourceVideo, "video/video_0.mp4");
    EXPECT_EQ(cam0.repairedVideo, "synced/video_0.mp4");
    EXPECT_EQ(cam0.sourceFramesCaptured, 23470);
    EXPECT_EQ(cam0.outputFrameCount, 23431);
    EXPECT_EQ(cam0.duplicatedFrameCount, 12);
    EXPECT_FALSE(cam0.skipped);
    EXPECT_TRUE(cam0.skipReason.isEmpty());
    EXPECT_TRUE(cam0.note.isEmpty());

    const auto& cam1 = result.cameras()[1];
    EXPECT_FALSE(cam1.skipped);
    EXPECT_FALSE(cam1.note.isEmpty());
    EXPECT_TRUE(cam1.note.contains("ended early"));

    const auto& cam5 = result.cameras()[2];
    EXPECT_EQ(cam5.index, 5);
    EXPECT_TRUE(cam5.sourceVideo.isEmpty());
    EXPECT_TRUE(cam5.repairedVideo.isEmpty());
    EXPECT_TRUE(cam5.skipped);
    EXPECT_FALSE(cam5.skipReason.isEmpty());
    EXPECT_TRUE(cam5.skipReason.contains("no video_N.mp4"));
}

TEST(SyncRepairResult, TotalDuplicatedFramesSumsOnlyNonSkippedCameras) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = SyncRepairResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    // 12 (cam0) + 15 (cam1); cam5's 0 (skipped) contributes nothing either way.
    EXPECT_EQ(result.total_duplicated_frames(), 27);
}

TEST(SyncRepairResult, SkippedCameraCountCountsOnlySkippedEntries) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = SyncRepairResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    EXPECT_EQ(result.skipped_camera_count(), 1);
}

TEST(SyncRepairResult, MissingFileIsInvalidNotCrashing) {
    const auto result = SyncRepairResult::load("Z:/does/not/exist/sync_repair.json");
    EXPECT_FALSE(result.is_valid());
    EXPECT_TRUE(result.cameras().isEmpty());
    EXPECT_EQ(result.total_duplicated_frames(), 0);
    EXPECT_EQ(result.skipped_camera_count(), 0);
}

TEST(SyncRepairResult, MalformedJsonIsInvalidNotCrashing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/sync_repair.json";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("{ not valid json");
    f.close();

    const auto result = SyncRepairResult::load(path);
    EXPECT_FALSE(result.is_valid());
}
