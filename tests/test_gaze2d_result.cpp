#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <cmath>

#include "analysis/gaze2d_result.hpp"

using mosaic::Gaze2dResult;

namespace {

// Matches the exact schema written by analysis/run_gaze2d.py's
// _write_results(). Frames at every 1000ms (frame_index 0,1,2,3,4), one
// frame with no face detected. face_box_px is [x1,y1,x2,y2] (bbox_xyxy).
const char* kFixtureJson = R"JSON(
{
  "schema": "mosaic-gaze2d-v1",
  "source_video": "video_0.mp4",
  "camera_index": 0,
  "min_confidence": 0.5,
  "frames": [
    {"frame_index": 0, "timestamp_ms": 0, "face_detected": true,
     "face_box_px": [400, 80, 500, 160], "gaze_dx": 0.10, "gaze_dy": -0.05},
    {"frame_index": 1, "timestamp_ms": 1000, "face_detected": true,
     "face_box_px": [402, 81, 502, 161], "gaze_dx": 0.12, "gaze_dy": -0.04},
    {"frame_index": 2, "timestamp_ms": 2000, "face_detected": false,
     "face_box_px": null, "gaze_dx": null, "gaze_dy": null},
    {"frame_index": 3, "timestamp_ms": 3000, "face_detected": true,
     "face_box_px": [405, 82, 505, 162], "gaze_dx": 0.05, "gaze_dy": 0.01},
    {"frame_index": 4, "timestamp_ms": 4000, "face_detected": true,
     "face_box_px": [406, 83, 506, 163], "gaze_dx": 0.03, "gaze_dy": 0.02}
  ],
  "summary": {"pct_frames_with_face": 0.8, "mean_gaze_dx": 0.075, "mean_gaze_dy": -0.015,
              "pct_on_target": 1.0}
}
)JSON";

QString write_fixture(const QString& dirPath) {
    const QString path = dirPath + "/video_0.gaze2d.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(kFixtureJson);
    return path;
}

} // namespace

TEST(Gaze2dResult, LoadsValidFileWithFullSchema) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = Gaze2dResult::load(write_fixture(dir.path()));

    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.source_video(), "video_0.mp4");
    EXPECT_DOUBLE_EQ(result.min_confidence(), 0.5);

    ASSERT_EQ(result.frames().size(), 5);
    const auto& f0 = result.frames()[0];
    EXPECT_TRUE(f0.faceDetected);
    EXPECT_EQ(f0.faceBoxPx, QRect(400, 80, 100, 80));
    EXPECT_DOUBLE_EQ(f0.gazeDx, 0.10);
    EXPECT_DOUBLE_EQ(f0.gazeDy, -0.05);

    // No-face frame parses cleanly, not dropped.
    EXPECT_FALSE(result.frames()[2].faceDetected);

    EXPECT_DOUBLE_EQ(result.pct_frames_with_face(), 0.8);
    ASSERT_TRUE(result.mean_gaze_dx().has_value());
    EXPECT_DOUBLE_EQ(*result.mean_gaze_dx(), 0.075);
    ASSERT_TRUE(result.mean_gaze_dy().has_value());
    EXPECT_DOUBLE_EQ(*result.mean_gaze_dy(), -0.015);
    ASSERT_TRUE(result.pct_on_target().has_value());
    EXPECT_DOUBLE_EQ(*result.pct_on_target(), 1.0);
}

TEST(Gaze2dResult, MissingFileIsInvalidNotCrashing) {
    const auto result = Gaze2dResult::load("Z:/does/not/exist.gaze2d.json");
    EXPECT_FALSE(result.is_valid());
    EXPECT_TRUE(result.frames().isEmpty());
    EXPECT_FALSE(result.mean_gaze_dx().has_value());
}

TEST(Gaze2dResult, NearestFrameHandlesGapsAndBoundaries) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = Gaze2dResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    EXPECT_EQ(result.nearest_frame(0)->frameIndex, 0);
    EXPECT_EQ(result.nearest_frame(3000)->frameIndex, 3);
    // Before the first frame clamps to the first.
    EXPECT_EQ(result.nearest_frame(-100)->frameIndex, 0);
    // After the last frame clamps to the last.
    EXPECT_EQ(result.nearest_frame(999999)->frameIndex, 4);
    // Mid-gap tie (500 is exactly between 0 and 1000) resolves to the
    // earlier frame, matching the "beforeDelta <= afterDelta" tie-break.
    EXPECT_EQ(result.nearest_frame(500)->frameIndex, 0);
}

TEST(Gaze2dResult, NearestFrameOnEmptyResultReturnsNull) {
    const Gaze2dResult result;
    EXPECT_EQ(result.nearest_frame(0), nullptr);
}

TEST(Gaze2dResult, NoFaceFrameHasDefaultGazeValues) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = Gaze2dResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    const auto& f2 = result.frames()[2];
    EXPECT_FALSE(f2.faceDetected);
    EXPECT_DOUBLE_EQ(f2.gazeDx, 0.0);
    EXPECT_DOUBLE_EQ(f2.gazeDy, 0.0);
    EXPECT_EQ(f2.faceBoxPx, QRect());
}
