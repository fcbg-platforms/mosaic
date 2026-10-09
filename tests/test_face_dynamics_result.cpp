#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <cmath>

#include "analysis/face_dynamics_result.hpp"

using mosaic::FaceDynamicsResult;

namespace {

// The schema analysis/run_face_dynamics.py writes (write_outputs()). Three
// frames at 20 ms, the second without a face, the third with a failed
// head-pose fit (null angles). Events are deliberately out of order.
const char* kFixtureJson = R"JSON(
{
 "schema": "mosaic-face-dynamics-v1",
 "source_video": "video_2.mp4",
 "camera_index": 2,
 "fps": 50.0,
 "settings": {"min_confidence": 0.5, "intrinsics": "nominal"},
 "annotated_video": "face_dynamics/video_2.face_dynamics.mp4",
 "summary": {
  "face_seen_s": 0.04, "face_seen_pct": 66.7,
  "blinks": {"count": 3, "per_minute": 18.5, "median_duration_ms": 160.0,
             "mean_interval_s": null, "long_closures": 1, "perclos_pct": 2.5},
  "expression": {"smiles": 2, "duchenne_smiles": 1, "smiling_pct": 12.0,
                 "brow_raises": 4, "brow_flashes": 3, "expressivity_mean": 0.051},
  "head": {"nods": 5, "shakes": 1, "mean_speed_deg_s": 14.2}
 },
 "events": [
  {"kind": "smile", "start_ms": 9000, "end_ms": 9800, "peak_ms": 9400, "duration_ms": 800.0, "peak": 0.8},
  {"kind": "blink", "start_ms": 5000, "end_ms": 5140, "peak_ms": 5060, "duration_ms": 160.0, "peak": 0.9},
  {"kind": "nod", "start_ms": 7000, "end_ms": 7900, "peak_ms": 7300, "duration_ms": 900.0, "peak": 8.5, "swings": 3}
 ],
 "frames": [
  {"frame_index": 0, "timestamp_ms": 5000, "face_detected": true, "face_box_px": [400, 200, 600, 460],
   "openness_left": 0.95, "openness_right": 1.01, "smile": 0.1, "brow_raise": 0.2,
   "expressivity": 0.03, "yaw": 5.5, "pitch": -2.0, "roll": 1.0, "head_speed": null},
  {"frame_index": 1, "timestamp_ms": 5020, "face_detected": false},
  {"frame_index": 2, "timestamp_ms": 5040, "face_detected": true, "face_box_px": [402, 201, 602, 461],
   "openness_left": 0.2, "openness_right": 0.25, "smile": 0.1, "brow_raise": 0.2,
   "expressivity": 0.03, "yaw": null, "pitch": null, "roll": null, "head_speed": null}
 ]
}
)JSON";

QString write_file(const QString& dir, const QByteArray& content) {
    const QString path = dir + "/video_2.face_dynamics.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(content);
    return path;
}

} // namespace

TEST(FaceDynamicsResult, LoadsFramesEventsAndSummary) {
    QTemporaryDir dir;
    const auto result = FaceDynamicsResult::load(write_file(dir.path(), kFixtureJson));
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.source_video(), "video_2.mp4");
    EXPECT_EQ(result.camera_index(), 2);
    EXPECT_DOUBLE_EQ(result.fps(), 50.0);
    EXPECT_EQ(result.annotated_video(), "face_dynamics/video_2.face_dynamics.mp4");

    ASSERT_EQ(result.frames().size(), 3);
    const auto& f0 = result.frames()[0];
    EXPECT_TRUE(f0.faceDetected);
    EXPECT_EQ(f0.faceBoxPx, QRect(400, 200, 200, 260));
    EXPECT_DOUBLE_EQ(f0.opennessRight, 1.01);
    EXPECT_DOUBLE_EQ(f0.yaw, 5.5);
    EXPECT_TRUE(std::isnan(f0.headSpeed)); // null in the file
    EXPECT_FALSE(result.frames()[1].faceDetected);
    EXPECT_TRUE(std::isnan(result.frames()[2].yaw));

    ASSERT_EQ(result.events().size(), 3);
    EXPECT_EQ(result.events()[0].kind, "blink"); // sorted by start
    EXPECT_EQ(result.events()[1].kind, "nod");
    EXPECT_EQ(result.events()[2].kind, "smile");
    EXPECT_DOUBLE_EQ(result.events()[0].durationMs, 160.0);
    EXPECT_EQ(result.events()[1].peakMs, 7300);

    const auto& s = result.summary();
    EXPECT_EQ(s.blinks, 3);
    ASSERT_TRUE(s.blinksPerMinute.has_value());
    EXPECT_DOUBLE_EQ(*s.blinksPerMinute, 18.5);
    EXPECT_FALSE(s.meanBlinkIntervalS.has_value()); // null in the file
    EXPECT_EQ(s.longClosures, 1);
    EXPECT_EQ(s.smiles, 2);
    EXPECT_EQ(s.duchenneSmiles, 1);
    EXPECT_EQ(s.browFlashes, 3);
    EXPECT_EQ(s.nods, 5);
    EXPECT_EQ(s.shakes, 1);
    ASSERT_TRUE(s.meanHeadSpeed.has_value());
    EXPECT_DOUBLE_EQ(*s.meanHeadSpeed, 14.2);
}

TEST(FaceDynamicsResult, NearestFrameByTimestamp) {
    QTemporaryDir dir;
    const auto result = FaceDynamicsResult::load(write_file(dir.path(), kFixtureJson));
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.nearest_frame(4000)->frameIndex, 0);
    EXPECT_EQ(result.nearest_frame(5021)->frameIndex, 1);
    EXPECT_EQ(result.nearest_frame(9000)->frameIndex, 2);
    EXPECT_EQ(FaceDynamicsResult().nearest_frame(0), nullptr);
}

TEST(FaceDynamicsResult, MissingOrForeignFileIsInvalid) {
    EXPECT_FALSE(FaceDynamicsResult::load("/no/such/file.json").is_valid());
    QTemporaryDir dir;
    const QString other = write_file(dir.path(), R"({"schema": "mosaic-gaze2d-v1", "frames": []})");
    EXPECT_FALSE(FaceDynamicsResult::load(other).is_valid());
}
