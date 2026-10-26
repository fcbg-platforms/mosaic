#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "analysis/hands_result.hpp"

using mosaic::HandsResult;

namespace {

// The schema analysis/run_hands.py writes (analyse_camera()), cut down.
const char* kFixtureJson = R"JSON(
{
 "schema": "mosaic-hands-v1",
 "camera_index": 4,
 "model": "rtmw",
 "stride": 2,
 "video_start_ms": 5000.0,
 "keypoint_names": ["wrist"],
 "hands": [
  {"subject_id": 3, "side": "left", "frames": 30, "seen_ms": 1200.0,
   "seen_spans": [[5000.0, 5800.0], [7000.0, 7400.0]], "median_size_px": 170.5,
   "mean_score": 0.41},
  {"subject_id": null, "side": "unknown", "frames": 2, "seen_ms": 80.0,
   "seen_spans": [[9000.0, 9080.0]], "median_size_px": 90.0, "mean_score": 0.6}
 ],
 "annotated_video": "hands/video_4.hands.mp4",
 "frames_file": "hands/video_4.hands.frames.json"
}
)JSON";

HandsResult load_fixture(QTemporaryDir& dir, const char* json) {
    const QString path = dir.filePath("video_4.hands.json");
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(json);
    f.close();
    return HandsResult::load(path);
}

} // namespace

TEST(HandsResult, ReadsTheModelAndEachHand) {
    QTemporaryDir dir;
    const HandsResult r = load_fixture(dir, kFixtureJson);
    ASSERT_TRUE(r.is_valid());
    EXPECT_EQ(r.model(), "rtmw");
    EXPECT_EQ(r.stride(), 2);
    EXPECT_DOUBLE_EQ(r.video_start_ms(), 5000.0);
    EXPECT_EQ(r.annotated_video(), "hands/video_4.hands.mp4");
    ASSERT_EQ(r.hands().size(), 2);
    const auto& left = r.hands()[0];
    EXPECT_EQ(left.subjectId, 3);
    EXPECT_EQ(left.side, "left");
    EXPECT_EQ(left.display_name(), "S3 left hand");
    EXPECT_DOUBLE_EQ(left.seenMs, 1200.0);
    EXPECT_DOUBLE_EQ(left.medianSizePx, 170.5);
    EXPECT_DOUBLE_EQ(left.meanScore, 0.41);
    ASSERT_EQ(left.seenSpans.size(), 2);
    EXPECT_DOUBLE_EQ(left.seenSpans[1].first, 7000.0);
    EXPECT_DOUBLE_EQ(left.seenSpans[1].second, 7400.0);
    // A hand no wrist was near: no person, no side.
    EXPECT_EQ(r.hands()[1].subjectId, -1);
    EXPECT_EQ(r.hands()[1].display_name(), "? hand");
}

TEST(HandsResult, AnotherSchemaOrNoFileIsInvalid) {
    QTemporaryDir dir;
    EXPECT_FALSE(load_fixture(dir, R"({"schema": "mosaic-objects-v1"})").is_valid());
    EXPECT_FALSE(HandsResult::load(dir.filePath("missing.json")).is_valid());
}
