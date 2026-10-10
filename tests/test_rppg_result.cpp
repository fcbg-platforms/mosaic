#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <cmath>

#include "analysis/rppg_result.hpp"

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
    EXPECT_FALSE(result.frames()[2].faceDetected); // no-face frame parses cleanly, not dropped

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

TEST(RppgResult, V1FileHasNoBeatsOrHrv) {
    QTemporaryDir dir;
    const auto result = RppgResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());
    EXPECT_TRUE(result.intervals().isEmpty());
    EXPECT_FALSE(result.hrv().has_value());
    EXPECT_TRUE(result.hrv_withheld().isEmpty());
    EXPECT_FALSE(result.hr_speaking().has_value());
}

TEST(RppgResult, LoadsBeatsAndHrvOfSchemaV2) {
    QTemporaryDir dir;
    const QString path = dir.path() + "/video_2.pos.rppg.json";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(R"JSON({
 "schema": "mosaic-rppg-v2", "source_video": "video_2.mp4", "backend": "pos",
 "windows": [{"start_ms": 1000000, "end_ms": 1010000, "bpm": 72.0, "smoothed_bpm": 72.0,
              "snr_db": 3.1, "valid_frame_fraction": 1.0}],
 "frames": [], "summary": {"mean_bpm": 72.0, "pct_windows_good": 1.0},
 "frame_rate": 50.0,
 "intervals": [{"t_ms": 1000800.0, "ibi_ms": 810.0, "nn": true},
               {"t_ms": 1002400.0, "ibi_ms": 1600.0, "nn": false}],
 "hrv": {"nn_count": 180, "mean_hr_bpm": 74.1, "sdnn_ms": 43.4, "rmssd_ms": 63.4,
         "pnn50_pct": 47.0, "lf_hf": 0.04, "timing_jitter_ms": 19.5,
         "rmssd_corrected_ms": 41.8, "sdnn_corrected_ms": 33.5},
 "hrv_withheld": [],
 "hrv_windows": [{"start_ms": 1000000, "end_ms": 1060000, "hr_bpm": 74.0, "rmssd_ms": 60.2,
                  "nn_count": 75},
                 {"start_ms": 1010000, "end_ms": 1070000, "hr_bpm": null, "rmssd_ms": null,
                  "nn_count": 1}],
 "by_state": {"speaking": {"beats": 70, "mean_hr_bpm": 78.5},
              "listening": {"beats": 80, "mean_hr_bpm": null}}
})JSON");
    f.close();
    const auto r = RppgResult::load(path);
    ASSERT_TRUE(r.is_valid());
    ASSERT_EQ(r.intervals().size(), 2);
    EXPECT_TRUE(r.intervals()[0].nn);
    EXPECT_FALSE(r.intervals()[1].nn);
    EXPECT_DOUBLE_EQ(r.intervals()[0].ibiMs, 810.0);
    ASSERT_TRUE(r.hrv().has_value());
    EXPECT_EQ(r.hrv()->nnCount, 180);
    EXPECT_DOUBLE_EQ(*r.hrv()->rmssdCorrectedMs, 41.8);
    EXPECT_DOUBLE_EQ(*r.hrv()->timingJitterMs, 19.5);
    ASSERT_EQ(r.hrv_windows().size(), 2);
    EXPECT_DOUBLE_EQ(r.hrv_windows()[0].rmssdMs, 60.2);
    EXPECT_TRUE(std::isnan(r.hrv_windows()[1].hrBpm));
    EXPECT_DOUBLE_EQ(*r.hr_speaking(), 78.5);
    EXPECT_FALSE(r.hr_listening().has_value());
}

TEST(RppgResult, WithheldHrvKeepsItsReasons) {
    QTemporaryDir dir;
    const QString path = dir.path() + "/v.pos.rppg.json";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(R"({"schema": "mosaic-rppg-v2", "windows": [], "frames": [], "summary": {},
               "hrv": null, "hrv_withheld": ["frame rate 13 fps is below 25 fps"]})");
    f.close();
    const auto r = RppgResult::load(path);
    ASSERT_TRUE(r.is_valid());
    EXPECT_FALSE(r.hrv().has_value());
    ASSERT_EQ(r.hrv_withheld().size(), 1);
    EXPECT_TRUE(r.hrv_withheld()[0].startsWith("frame rate"));
}
