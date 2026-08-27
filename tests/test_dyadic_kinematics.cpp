#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <cmath>

#include "analysis/dyadic_kinematics.hpp"
#include "analysis/skeleton3d_result.hpp"

using mosaic::compute_dyadic_kinematics;
using mosaic::Skeleton3DResult;

namespace {

QString write_json(const QString& dirPath, const QString& name, const char* json) {
    const QString path = dirPath + "/" + name;
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(json);
    return path;
}

// ── Facing-direction fixtures ───────────────────────────────────────────────
//
// Hand-verified: shoulders separated along local Y, spine along local Z,
// nose offset from the shoulder midpoint indicates the intended facing axis
// — cross(spine, shoulders) always lands on the axis orthogonal to both
// (here, X for a Y/Z-only torso, or Y for an X/Z-only torso), and the
// nose-relative sign flip resolves the raw product's handedness regardless
// of which way it initially points. See dyadic_kinematics.cpp's
// facing_forward() and the plan's own hand-derivation for the exact
// arithmetic these fixtures were built from.

// Two people 1000mm apart along X, each facing the other — A's nose is +X of
// its own shoulders, B's nose is -X of its own (mirrored) shoulders.
const char* kFaceToFaceJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1", "source_videos": [], "cameras": [],
  "keypoint_names": ["nose", "left_shoulder", "right_shoulder", "left_hip", "right_hip"],
  "skeleton_edges": [], "master_fps": 25.0, "params": {},
  "frames": [
    {
      "tick": 0, "timestamp_ns": 1000000000,
      "people": [
        {
          "track_id": 0, "num_contributing_cameras": 2, "source_cameras": [0, 1],
          "keypoints_room": [[100.0, 0.0, 1500.0], [0.0, -150.0, 1400.0], [0.0, 150.0, 1400.0],
                             [0.0, -100.0, 900.0], [0.0, 100.0, 900.0]],
          "keypoints_valid": [true, true, true, true, true],
          "reprojection_error_px": [1.0, 1.0, 1.0, 1.0, 1.0]
        },
        {
          "track_id": 1, "num_contributing_cameras": 2, "source_cameras": [0, 1],
          "keypoints_room": [[900.0, 0.0, 1500.0], [1000.0, 150.0, 1400.0],
                             [1000.0, -150.0, 1400.0], [1000.0, 100.0, 900.0],
                             [1000.0, -100.0, 900.0]],
          "keypoints_valid": [true, true, true, true, true],
          "reprojection_error_px": [1.0, 1.0, 1.0, 1.0, 1.0]
        }
      ]
    }
  ]
}
)JSON";

// Two people, both facing +X (B offset +500mm along X, same orientation).
const char* kSameDirectionJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1", "source_videos": [], "cameras": [],
  "keypoint_names": ["nose", "left_shoulder", "right_shoulder", "left_hip", "right_hip"],
  "skeleton_edges": [], "master_fps": 25.0, "params": {},
  "frames": [
    {
      "tick": 0, "timestamp_ns": 1000000000,
      "people": [
        {
          "track_id": 0, "num_contributing_cameras": 2, "source_cameras": [0, 1],
          "keypoints_room": [[100.0, 0.0, 1500.0], [0.0, -150.0, 1400.0], [0.0, 150.0, 1400.0],
                             [0.0, -100.0, 900.0], [0.0, 100.0, 900.0]],
          "keypoints_valid": [true, true, true, true, true],
          "reprojection_error_px": [1.0, 1.0, 1.0, 1.0, 1.0]
        },
        {
          "track_id": 1, "num_contributing_cameras": 2, "source_cameras": [0, 1],
          "keypoints_room": [[600.0, 0.0, 1500.0], [500.0, -150.0, 1400.0],
                             [500.0, 150.0, 1400.0], [500.0, -100.0, 900.0],
                             [500.0, 100.0, 900.0]],
          "keypoints_valid": [true, true, true, true, true],
          "reprojection_error_px": [1.0, 1.0, 1.0, 1.0, 1.0]
        }
      ]
    }
  ]
}
)JSON";

// A faces +X; B faces +Y (shoulders separated along X instead of Y so the
// cross product lands on the Y axis, nose offset +Y from the shoulder
// midpoint to select that direction) — orthogonal to A's facing.
const char* kPerpendicularJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1", "source_videos": [], "cameras": [],
  "keypoint_names": ["nose", "left_shoulder", "right_shoulder", "left_hip", "right_hip"],
  "skeleton_edges": [], "master_fps": 25.0, "params": {},
  "frames": [
    {
      "tick": 0, "timestamp_ns": 1000000000,
      "people": [
        {
          "track_id": 0, "num_contributing_cameras": 2, "source_cameras": [0, 1],
          "keypoints_room": [[100.0, 0.0, 1500.0], [0.0, -150.0, 1400.0], [0.0, 150.0, 1400.0],
                             [0.0, -100.0, 900.0], [0.0, 100.0, 900.0]],
          "keypoints_valid": [true, true, true, true, true],
          "reprojection_error_px": [1.0, 1.0, 1.0, 1.0, 1.0]
        },
        {
          "track_id": 1, "num_contributing_cameras": 2, "source_cameras": [0, 1],
          "keypoints_room": [[2000.0, 100.0, 1500.0], [1850.0, 0.0, 1400.0],
                             [2150.0, 0.0, 1400.0], [2000.0, -100.0, 900.0],
                             [2000.0, 100.0, 900.0]],
          "keypoints_valid": [true, true, true, true, true],
          "reprojection_error_px": [1.0, 1.0, 1.0, 1.0, 1.0]
        }
      ]
    }
  ]
}
)JSON";

// Hips only (no nose/shoulders in keypoint_names at all) — doubles as the
// "missing facing keypoints" test. A stationary at hip-midpoint (0,0,900);
// B's hip-midpoint moves along X: 1000mm, 900mm, 850mm distance at t=0,
// 100ms, 200ms.
const char* kApproachRateJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1", "source_videos": [], "cameras": [],
  "keypoint_names": ["left_hip", "right_hip"],
  "skeleton_edges": [], "master_fps": 25.0, "params": {},
  "frames": [
    {
      "tick": 0, "timestamp_ns": 0,
      "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[0.0, -50.0, 900.0], [0.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[1000.0, -50.0, 900.0], [1000.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}
      ]
    },
    {
      "tick": 1, "timestamp_ns": 100000000,
      "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[0.0, -50.0, 900.0], [0.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[900.0, -50.0, 900.0], [900.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}
      ]
    },
    {
      "tick": 2, "timestamp_ns": 200000000,
      "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[0.0, -50.0, 900.0], [0.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[850.0, -50.0, 900.0], [850.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}
      ]
    }
  ]
}
)JSON";

// Same shape as kApproachRateJson but track 0 is entirely ABSENT from tick
// 2's people array (a real tracking gap, not just an invalid keypoint) —
// distanceMm/approachRateMmPerS must skip it cleanly, using the real
// elapsed time between the two nearest ticks where both are present (0.1s
// -> 0.3s, i.e. 0.2s, not an assumed 0.1s tick period).
const char* kGapHandlingJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1", "source_videos": [], "cameras": [],
  "keypoint_names": ["left_hip", "right_hip"],
  "skeleton_edges": [], "master_fps": 25.0, "params": {},
  "frames": [
    {
      "tick": 0, "timestamp_ns": 0,
      "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[0.0, -50.0, 900.0], [0.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[1000.0, -50.0, 900.0], [1000.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}
      ]
    },
    {
      "tick": 1, "timestamp_ns": 100000000,
      "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[0.0, -50.0, 900.0], [0.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[900.0, -50.0, 900.0], [900.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}
      ]
    },
    {
      "tick": 2, "timestamp_ns": 200000000,
      "people": [
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[800.0, -50.0, 900.0], [800.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}
      ]
    },
    {
      "tick": 3, "timestamp_ns": 300000000,
      "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[0.0, -50.0, 900.0], [0.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[700.0, -50.0, 900.0], [700.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}
      ]
    }
  ]
}
)JSON";

// 5 ticks, both people's hip-midpoint X-deltas are IDENTICAL in shape
// (10,20,30,40 -> speeds 100,200,300,400 mm/s), so their speed sequences are
// perfectly positively correlated once enough paired samples exist.
const char* kCongruentPositiveJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1", "source_videos": [], "cameras": [],
  "keypoint_names": ["left_hip", "right_hip"],
  "skeleton_edges": [], "master_fps": 25.0, "params": {},
  "frames": [
    {"tick": 0, "timestamp_ns": 0, "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[0.0, -50.0, 900.0], [0.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[2000.0, -50.0, 900.0], [2000.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}]},
    {"tick": 1, "timestamp_ns": 100000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[10.0, -50.0, 900.0], [10.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[2010.0, -50.0, 900.0], [2010.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}]},
    {"tick": 2, "timestamp_ns": 200000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[30.0, -50.0, 900.0], [30.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[2030.0, -50.0, 900.0], [2030.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}]},
    {"tick": 3, "timestamp_ns": 300000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[60.0, -50.0, 900.0], [60.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[2060.0, -50.0, 900.0], [2060.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}]},
    {"tick": 4, "timestamp_ns": 400000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[100.0, -50.0, 900.0], [100.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[2100.0, -50.0, 900.0], [2100.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}]}
  ]
}
)JSON";

// Same as kCongruentPositiveJson but B's per-step deltas are the reverse
// order of A's (40,30,20,10 -> speeds 400,300,200,100) — a perfect linear
// NEGATIVE relationship (speedB = 500 - speedA at every paired sample).
const char* kCongruentNegativeJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1", "source_videos": [], "cameras": [],
  "keypoint_names": ["left_hip", "right_hip"],
  "skeleton_edges": [], "master_fps": 25.0, "params": {},
  "frames": [
    {"tick": 0, "timestamp_ns": 0, "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[0.0, -50.0, 900.0], [0.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[2000.0, -50.0, 900.0], [2000.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}]},
    {"tick": 1, "timestamp_ns": 100000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[10.0, -50.0, 900.0], [10.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[2040.0, -50.0, 900.0], [2040.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}]},
    {"tick": 2, "timestamp_ns": 200000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[30.0, -50.0, 900.0], [30.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[2070.0, -50.0, 900.0], [2070.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}]},
    {"tick": 3, "timestamp_ns": 300000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[60.0, -50.0, 900.0], [60.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[2090.0, -50.0, 900.0], [2090.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}]},
    {"tick": 4, "timestamp_ns": 400000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[100.0, -50.0, 900.0], [100.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]},
        {"track_id": 1, "num_contributing_cameras": 1, "source_cameras": [0],
         "keypoints_room": [[2100.0, -50.0, 900.0], [2100.0, 50.0, 900.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [1.0, 1.0]}]}
  ]
}
)JSON";

} // namespace

TEST(DyadicKinematics, FaceToFaceGivesFacingCosineNearMinusOne) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = Skeleton3DResult::load(write_json(dir.path(), "a.json", kFaceToFaceJson));
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 1);
    ASSERT_EQ(series.samples.size(), 1);
    EXPECT_NEAR(series.samples[0].distanceMm, 1000.0, 1e-6);
    ASSERT_FALSE(std::isnan(series.samples[0].facingCosine));
    EXPECT_NEAR(series.samples[0].facingCosine, -1.0, 1e-6);
    EXPECT_NEAR(series.stats.meanFacingCosine, -1.0, 1e-6);
}

TEST(DyadicKinematics, SameDirectionGivesFacingCosineNearPlusOne) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result =
        Skeleton3DResult::load(write_json(dir.path(), "a.json", kSameDirectionJson));
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 1);
    ASSERT_EQ(series.samples.size(), 1);
    ASSERT_FALSE(std::isnan(series.samples[0].facingCosine));
    EXPECT_NEAR(series.samples[0].facingCosine, 1.0, 1e-6);
}

TEST(DyadicKinematics, PerpendicularGivesFacingCosineNearZero) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result =
        Skeleton3DResult::load(write_json(dir.path(), "a.json", kPerpendicularJson));
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 1);
    ASSERT_EQ(series.samples.size(), 1);
    ASSERT_FALSE(std::isnan(series.samples[0].facingCosine));
    EXPECT_NEAR(series.samples[0].facingCosine, 0.0, 1e-6);
}

TEST(DyadicKinematics, ApproachRateUsesRealElapsedTimeBetweenSamples) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = Skeleton3DResult::load(write_json(dir.path(), "a.json", kApproachRateJson));
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 1);
    ASSERT_EQ(series.samples.size(), 3);

    EXPECT_NEAR(series.samples[0].distanceMm, 1000.0, 1e-6);
    EXPECT_NEAR(series.samples[1].distanceMm, 900.0, 1e-6);
    EXPECT_NEAR(series.samples[2].distanceMm, 850.0, 1e-6);

    EXPECT_TRUE(std::isnan(series.samples[0].approachRateMmPerS)); // no prior sample
    EXPECT_NEAR(series.samples[1].approachRateMmPerS, -1000.0, 1e-3);
    EXPECT_NEAR(series.samples[2].approachRateMmPerS, -500.0, 1e-3);

    // "left_hip"/"right_hip" only in keypoint_names — facingCosine must stay
    // NaN throughout rather than crash or misread the wrong index.
    for (const auto& s : series.samples) {
        EXPECT_TRUE(std::isnan(s.facingCosine));
    }
    EXPECT_TRUE(std::isnan(series.stats.meanFacingCosine));
}

TEST(DyadicKinematics, GapWhereOnePersonIsAbsentLeavesDistanceNaNAndSkipsCleanly) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = Skeleton3DResult::load(write_json(dir.path(), "a.json", kGapHandlingJson));
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 1);
    ASSERT_EQ(series.samples.size(), 4);

    EXPECT_NEAR(series.samples[0].distanceMm, 1000.0, 1e-6);
    EXPECT_NEAR(series.samples[1].distanceMm, 900.0, 1e-6);
    EXPECT_TRUE(std::isnan(series.samples[2].distanceMm)); // track 0 absent this tick
    EXPECT_NEAR(series.samples[3].distanceMm, 700.0, 1e-6);

    EXPECT_TRUE(std::isnan(series.samples[0].approachRateMmPerS));
    EXPECT_NEAR(series.samples[1].approachRateMmPerS, -1000.0, 1e-3);
    EXPECT_TRUE(std::isnan(series.samples[2].approachRateMmPerS));
    // Bridges the gap using the real 0.2s elapsed between tick 1 (t=0.1s)
    // and tick 3 (t=0.3s), NOT an assumed 0.1s tick period.
    EXPECT_NEAR(series.samples[3].approachRateMmPerS, -1000.0, 1e-3);

    EXPECT_NEAR(series.stats.pctTicksBothPresent, 75.0, 1e-6); // 3 of 4 ticks
}

TEST(DyadicKinematics, PerfectlyCorrelatedSpeedsGiveCorrelationNearPlusOne) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result =
        Skeleton3DResult::load(write_json(dir.path(), "a.json", kCongruentPositiveJson));
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 1, /*congruentMotionWindow=*/30);
    ASSERT_EQ(series.samples.size(), 5);

    // Fewer than 3 paired (both-speed-defined) samples exist before tick 3.
    EXPECT_TRUE(std::isnan(series.samples[0].congruentMotionCorr));
    EXPECT_TRUE(std::isnan(series.samples[1].congruentMotionCorr));
    EXPECT_TRUE(std::isnan(series.samples[2].congruentMotionCorr));
    ASSERT_FALSE(std::isnan(series.samples[3].congruentMotionCorr));
    EXPECT_NEAR(series.samples[3].congruentMotionCorr, 1.0, 1e-6);
    ASSERT_FALSE(std::isnan(series.samples[4].congruentMotionCorr));
    EXPECT_NEAR(series.samples[4].congruentMotionCorr, 1.0, 1e-6);
    EXPECT_NEAR(series.stats.meanCongruentMotionCorr, 1.0, 1e-6);
}

TEST(DyadicKinematics, AntiCorrelatedSpeedsGiveCorrelationNearMinusOne) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result =
        Skeleton3DResult::load(write_json(dir.path(), "a.json", kCongruentNegativeJson));
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 1, /*congruentMotionWindow=*/30);
    ASSERT_EQ(series.samples.size(), 5);

    ASSERT_FALSE(std::isnan(series.samples[3].congruentMotionCorr));
    EXPECT_NEAR(series.samples[3].congruentMotionCorr, -1.0, 1e-6);
    ASSERT_FALSE(std::isnan(series.samples[4].congruentMotionCorr));
    EXPECT_NEAR(series.samples[4].congruentMotionCorr, -1.0, 1e-6);
}

TEST(DyadicKinematics, InvalidResultProducesEmptySeries) {
    const Skeleton3DResult result;
    const auto series = compute_dyadic_kinematics(result, 0, 1);
    EXPECT_TRUE(series.samples.isEmpty());
    EXPECT_TRUE(std::isnan(series.stats.meanDistanceMm));
}
