#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "analysis/gaze_fusion_result.hpp"

using mosaic::GazeFusionResult;

namespace {

// Matches the exact schema written by analysis/run_gaze_fusion.py's
// _write_results()/_fuse_ticks(). Tick 0 has 2 contributing cameras
// (triangulated, has a target point), tick 1 has 1 (not triangulated, no
// target), tick 2 has 0 (no fused ray at all) — covering every branch of
// _fuse_ticks()'s contribution-count logic.
const char* kFixtureJson = R"JSON(
{
  "schema": "mosaic-gaze-fusion-v1",
  "source_videos": ["video/video_0.mp4", "video/video_1.mp4"],
  "cameras": [
    {"index": 0, "position_room": [0.0, 0.0, 0.0]},
    {"index": 1, "position_room": [500.0, 0.0, 100.0]}
  ],
  "plane": {"defined": true, "point": [0.0, 0.0, 1000.0], "normal": [0.0, 0.0, 1.0]},
  "master_fps": 25.0,
  "frames": [
    {
      "tick": 0, "timestamp_ns": 1000000000,
      "num_cameras": 2, "is_triangulated": true,
      "fused_origin_room": [10.0, 20.0, 30.0],
      "fused_direction_room": [0.0, 0.0, 1.0],
      "residual_rms_mm": 12.3,
      "target_point_room": [10.0, 20.0, 1000.0],
      "per_camera": [
        {"camera_index": 0, "face_box_px": [10.0, 10.0, 100.0, 100.0],
         "gaze_dx": 0.1, "gaze_dy": -0.2,
         "origin_room": [10.0, 20.0, 30.0], "direction_room": [0.0, 0.0, 1.0],
         "confidence": 1.0},
        {"camera_index": 1, "face_box_px": [20.0, 20.0, 110.0, 110.0],
         "gaze_dx": 0.05, "gaze_dy": -0.15,
         "origin_room": [12.0, 18.0, 28.0], "direction_room": [0.01, 0.0, 0.999],
         "confidence": 1.0}
      ]
    },
    {
      "tick": 1, "timestamp_ns": 1040000000,
      "num_cameras": 1, "is_triangulated": false,
      "fused_origin_room": [11.0, 21.0, 31.0],
      "fused_direction_room": [0.0, 0.0, 1.0],
      "residual_rms_mm": null,
      "per_camera": [
        {"camera_index": 0, "face_box_px": [11.0, 11.0, 101.0, 101.0],
         "gaze_dx": 0.12, "gaze_dy": -0.18,
         "origin_room": [11.0, 21.0, 31.0], "direction_room": [0.0, 0.0, 1.0],
         "confidence": 1.0}
      ]
    },
    {
      "tick": 2, "timestamp_ns": 1080000000,
      "num_cameras": 0, "is_triangulated": false,
      "per_camera": []
    }
  ]
}
)JSON";

QString write_fixture(const QString& dirPath) {
    const QString path = dirPath + "/gaze_fusion.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(kFixtureJson);
    return path;
}

} // namespace

TEST(GazeFusionResult, LoadsValidFileWithFullSchema) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = GazeFusionResult::load(write_fixture(dir.path()));

    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.source_videos(), QStringList({"video/video_0.mp4", "video/video_1.mp4"}));
    ASSERT_EQ(result.cameras().size(), 2);
    EXPECT_EQ(result.cameras()[1].index, 1);
    EXPECT_DOUBLE_EQ(result.cameras()[1].positionRoom[0], 500.0);
    EXPECT_TRUE(result.plane_defined());
    EXPECT_DOUBLE_EQ(result.plane_point()[2], 1000.0);
    EXPECT_DOUBLE_EQ(result.plane_normal()[2], 1.0);
    EXPECT_DOUBLE_EQ(result.master_fps(), 25.0);

    ASSERT_EQ(result.frames().size(), 3);

    const auto& f0 = result.frames()[0];
    EXPECT_EQ(f0.tick, 0);
    EXPECT_EQ(f0.timestampNs, 1000000000);
    EXPECT_EQ(f0.numCameras, 2);
    EXPECT_TRUE(f0.isTriangulated);
    EXPECT_DOUBLE_EQ(f0.fusedOriginRoom[0], 10.0);
    EXPECT_DOUBLE_EQ(f0.residualRmsMm, 12.3);
    EXPECT_TRUE(f0.hasTarget);
    EXPECT_DOUBLE_EQ(f0.targetPointRoom[2], 1000.0);
    ASSERT_EQ(f0.perCamera.size(), 2);
}

TEST(GazeFusionResult, MissingFileIsInvalidNotCrashing) {
    const auto result = GazeFusionResult::load("Z:/does/not/exist/gaze_fusion.json");
    EXPECT_FALSE(result.is_valid());
    EXPECT_TRUE(result.frames().isEmpty());
}

TEST(GazeFusionResult, NearestFrameHandlesGapsAndBoundaries) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = GazeFusionResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    // Exact matches.
    EXPECT_EQ(result.nearest_frame(1000000000)->tick, 0);
    EXPECT_EQ(result.nearest_frame(1040000000)->tick, 1);
    EXPECT_EQ(result.nearest_frame(1080000000)->tick, 2);

    // Before the first frame clamps to the first.
    EXPECT_EQ(result.nearest_frame(0)->tick, 0);

    // After the last frame clamps to the last.
    EXPECT_EQ(result.nearest_frame(9999999999)->tick, 2);

    // Mid-gap exact ties resolve to the earlier frame.
    EXPECT_EQ(result.nearest_frame(1020000000)->tick, 0); // midpoint of tick 0/1
    EXPECT_EQ(result.nearest_frame(1060000000)->tick, 1); // midpoint of tick 1/2
}

TEST(GazeFusionResult, NearestFrameOnEmptyResultReturnsNull) {
    const GazeFusionResult result;
    EXPECT_EQ(result.nearest_frame(0), nullptr);
}

TEST(GazeFusionResult, HandlesZeroAndOneCameraFramesWithoutTarget) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = GazeFusionResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    const auto& oneCam = result.frames()[1];
    EXPECT_EQ(oneCam.numCameras, 1);
    EXPECT_FALSE(oneCam.isTriangulated);
    EXPECT_DOUBLE_EQ(oneCam.residualRmsMm, -1.0); // null in JSON -> -1 sentinel
    EXPECT_FALSE(oneCam.hasTarget);

    const auto& zeroCam = result.frames()[2];
    EXPECT_EQ(zeroCam.numCameras, 0);
    EXPECT_FALSE(zeroCam.isTriangulated);
    EXPECT_FALSE(zeroCam.hasTarget);
    EXPECT_TRUE(zeroCam.perCamera.isEmpty());
}

TEST(GazeFusionResult, ParsesPerCameraFaceBoxAndGazeOffsetForOverlay) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = GazeFusionResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    const auto& cam0 = result.frames()[0].perCamera[0];
    EXPECT_EQ(cam0.cameraIndex, 0);
    EXPECT_DOUBLE_EQ(cam0.faceBoxPx.left(), 10.0);
    EXPECT_DOUBLE_EQ(cam0.faceBoxPx.top(), 10.0);
    EXPECT_DOUBLE_EQ(cam0.gazeDx, 0.1);
    EXPECT_DOUBLE_EQ(cam0.gazeDy, -0.2);
    EXPECT_DOUBLE_EQ(cam0.originRoom[1], 20.0);
    EXPECT_DOUBLE_EQ(cam0.directionRoom[2], 1.0);
    EXPECT_DOUBLE_EQ(cam0.confidence, 1.0);
}
