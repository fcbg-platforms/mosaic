#include "analysis/rppg_result.hpp"
#include <gtest/gtest.h>
#include <QFile>
#include <QTemporaryDir>
#include <cmath>

using mosaic::RppgResult;

namespace {

// Matches the exact schema written by analysis/run_rppg.py's
// _write_results(). Windows at 0/2000/4000ms (as if hop_sec=2), frames at
// every 1000ms (frame_index 0,1,2,3,4), one frame with no face detected.
const char* kFixtureJson = R"JSON(
{
  "schema": "mosaic-rppg-v1",
  "source_video": "video_0.mp4",
  "backend": "pos",
  "window_sec": 10.0,
  "hop_sec": 2.0,
  "camera_index": 0,
  "windows": [
    {"start_ms": 0, "end_ms": 10000, "bpm": 71.4, "smoothed_bpm": 71.4,
     "snr_db": 6.2, "valid_frame_fraction": 0.97},
    {"start_ms": 2000, "end_ms": 12000, "bpm": 72.1, "smoothed_bpm": 71.7,
     "snr_db": 5.8, "valid_frame_fraction": 0.95},
    {"start_ms": 4000, "end_ms": 14000, "bpm": null, "smoothed_bpm": null,
     "snr_db": null, "valid_frame_fraction": 0.2}
  ],
  "frames": [
    {"frame_index": 0, "timestamp_ms": 0, "face_detected": true, "roi_bbox_px": [10, 20, 50, 40]},
    {"frame_index": 1, "timestamp_ms": 1000, "face_detected": true, "roi_bbox_px": [12, 21, 50, 40]},
    {"frame_index": 2, "timestamp_ms": 2000, "face_detected": false, "roi_bbox_px": null},
    {"frame_index": 3, "timestamp_ms": 3000, "face_detected": true, "roi_bbox_px": [14, 22, 51, 41]},
    {"frame_index": 4, "timestamp_ms": 4000, "face_detected": true, "roi_bbox_px": [15, 23, 51, 41]}
  ],
  "summary": {"mean_bpm": 71.75, "median_bpm": 71.75, "min_bpm": 71.4, "max_bpm": 72.1,
              "pct_windows_good": 0.667}
}
)JSON";

QString write_fixture(const QString& dirPath) {
    const QString path = dirPath + "/video_0.pos.rppg.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(kFixtureJson);
    return path;
}

} // namespace

TEST(RppgResult, LoadsValidFileWithFullSchema) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = RppgResult::load(write_fixture(dir.path()));

    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.source_video(), "video_0.mp4");
    EXPECT_EQ(result.backend(), "pos");
    EXPECT_DOUBLE_EQ(result.window_sec(), 10.0);
    EXPECT_DOUBLE_EQ(result.hop_sec(), 2.0);

    ASSERT_EQ(result.windows().size(), 3);
    const auto& w0 = result.windows()[0];
    EXPECT_EQ(w0.startMs, 0);
    EXPECT_EQ(w0.endMs, 10000);
    EXPECT_DOUBLE_EQ(w0.bpm, 71.4);
    EXPECT_DOUBLE_EQ(w0.smoothedBpm, 71.4);
    EXPECT_DOUBLE_EQ(w0.snrDb, 6.2);
    EXPECT_DOUBLE_EQ(w0.validFrameFraction, 0.97);

    // Third window's bpm/smoothedBpm/snrDb are JSON null -> NaN, never
    // fabricated as 0 or some other placeholder.
    const auto& w2 = result.windows()[2];
    EXPECT_TRUE(std::isnan(w2.bpm));
    EXPECT_TRUE(std::isnan(w2.smoothedBpm));
    EXPECT_TRUE(std::isnan(w2.snrDb));

    ASSERT_EQ(result.frames().size(), 5);
    EXPECT_TRUE(result.frames()[0].faceDetected);
    EXPECT_EQ(result.frames()[0].roiBboxPx, QRect(10, 20, 50, 40));
    EXPECT_FALSE(result.frames()[2].faceDetected);   // no-face frame parses cleanly, not dropped

    ASSERT_TRUE(result.mean_bpm().has_value());
    EXPECT_DOUBLE_EQ(*result.mean_bpm(), 71.75);
    ASSERT_TRUE(result.median_bpm().has_value());
    ASSERT_TRUE(result.min_bpm().has_value());
    ASSERT_TRUE(result.max_bpm().has_value());
    EXPECT_DOUBLE_EQ(result.pct_windows_good(), 0.667);
}

TEST(RppgResult, MissingFileIsInvalidNotCrashing) {
    const auto result = RppgResult::load("Z:/does/not/exist.rppg.json");
    EXPECT_FALSE(result.is_valid());
    EXPECT_TRUE(result.windows().isEmpty());
    EXPECT_TRUE(result.frames().isEmpty());
    EXPECT_FALSE(result.mean_bpm().has_value());
}

TEST(RppgResult, NearestWindowHandlesGapsAndBoundaries) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = RppgResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    EXPECT_EQ(result.nearest_window(0)->startMs, 0);
    EXPECT_EQ(result.nearest_window(2000)->startMs, 2000);
    EXPECT_EQ(result.nearest_window(4000)->startMs, 4000);

    // Before the first window clamps to the first.
    EXPECT_EQ(result.nearest_window(-500)->startMs, 0);
    // After the last window clamps to the last.
    EXPECT_EQ(result.nearest_window(100000)->startMs, 4000);

    // Mid-gap tie (1000 is exactly between 0 and 2000) resolves to the
    // earlier window, matching the "beforeDelta <= afterDelta" tie-break.
    EXPECT_EQ(result.nearest_window(1000)->startMs, 0);
}

TEST(RppgResult, NearestFrameHandlesGapsAndBoundaries) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = RppgResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    EXPECT_EQ(result.nearest_frame(0)->frameIndex, 0);
    EXPECT_EQ(result.nearest_frame(3000)->frameIndex, 3);
    EXPECT_EQ(result.nearest_frame(-100)->frameIndex, 0);
    EXPECT_EQ(result.nearest_frame(999999)->frameIndex, 4);
}

TEST(RppgResult, NearestWindowAndFrameOnEmptyResultReturnNull) {
    const RppgResult result;
    EXPECT_EQ(result.nearest_window(0), nullptr);
    EXPECT_EQ(result.nearest_frame(0), nullptr);
}
