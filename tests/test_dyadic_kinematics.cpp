#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <cmath>

#include "analysis/dyadic_kinematics.hpp"
#include "analysis/skeleton3d_result.hpp"

using mosaic::compute_dyadic_kinematics;
using mosaic::Skeleton3DResult;

namespace {

// One tick, 4 people, keypoint_names = [nose, left_shoulder, right_shoulder,
// left_hip, right_hip] — every geometric value below was hand-derived (see
// the dyadic-kinematics plan) so the expected facingCosine/distance values
// are exact, not approximate:
//   track 0 (A): torso at x=0,  forward = +X
//   track 1 (B): torso at x=1000, forward = -X  -> face-to-face with A,
//                distance 1000mm, facingCosine = -1
//   track 2 (C): torso at y=300 (beside A), forward = +X (same as A)
//                -> side-by-side, distance 300mm, facingCosine = +1
//   track 3 (D): torso at y=1000, forward = +Y (perpendicular to A)
//                -> distance 1000mm, facingCosine = 0
const char* kFacingFixtureJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1",
  "source_videos": [], "cameras": [],
  "keypoint_names": ["nose", "left_shoulder", "right_shoulder", "left_hip", "right_hip"],
  "skeleton_edges": [], "master_fps": 25.0,
  "frames": [
    {
      "tick": 0, "timestamp_ns": 0,
      "people": [
        {
          "track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
          "keypoints_room": [[100.0, 0.0, 600.0], [0.0, -150.0, 500.0], [0.0, 150.0, 500.0],
                             [0.0, -100.0, 0.0], [0.0, 100.0, 0.0]],
          "keypoints_valid": [true, true, true, true, true],
          "reprojection_error_px": [0, 0, 0, 0, 0], "reprojected_px": {}
        },
        {
          "track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
          "keypoints_room": [[900.0, 0.0, 600.0], [1000.0, 150.0, 500.0], [1000.0, -150.0, 500.0],
                             [1000.0, -100.0, 0.0], [1000.0, 100.0, 0.0]],
          "keypoints_valid": [true, true, true, true, true],
          "reprojection_error_px": [0, 0, 0, 0, 0], "reprojected_px": {}
        },
        {
          "track_id": 2, "num_contributing_cameras": 0, "source_cameras": [],
          "keypoints_room": [[100.0, 300.0, 600.0], [0.0, 150.0, 500.0], [0.0, 450.0, 500.0],
                             [0.0, 200.0, 0.0], [0.0, 400.0, 0.0]],
          "keypoints_valid": [true, true, true, true, true],
          "reprojection_error_px": [0, 0, 0, 0, 0], "reprojected_px": {}
        },
        {
          "track_id": 3, "num_contributing_cameras": 0, "source_cameras": [],
          "keypoints_room": [[0.0, 1100.0, 600.0], [-150.0, 1000.0, 500.0], [150.0, 1000.0, 500.0],
                             [-100.0, 1000.0, 0.0], [100.0, 1000.0, 0.0]],
          "keypoints_valid": [true, true, true, true, true],
          "reprojection_error_px": [0, 0, 0, 0, 0], "reprojected_px": {}
        }
      ]
    }
  ]
}
)JSON";

// keypoint_names deliberately omits "nose"/"left_shoulder"/"right_shoulder"
// (facing-related keypoints don't exist at all on this "skeleton") so
// facingCosine must stay NaN throughout while distance/approach-rate still
// compute normally — the direct regression test for the resolve-by-name
// defensive design. Track 0 (A) sits still at hipMid x=0 for all 5 ticks;
// track 1 (B) moves, with an invalid-hip gap at tick 2:
//   tick0 t=0s:  distA-B = 2000mm
//   tick1 t=1s:  distA-B = 1000mm  -> approachRate = (1000-2000)/1s = -1000 mm/s
//   tick2 t=2s:  B's hips invalid  -> distance NaN (gap)
//   tick3 t=3s:  distA-B = 500mm   -> approachRate uses REAL dt vs tick1
//                                     (2s, not 1 nominal tick): (500-1000)/2 = -250 mm/s
//   tick4 t=4s:  distA-B = 2500mm  -> approachRate = (2500-500)/1s = +2000 mm/s (retreating)
const char* kApproachRateFixtureJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1",
  "source_videos": [], "cameras": [],
  "keypoint_names": ["left_hip", "right_hip"],
  "skeleton_edges": [], "master_fps": 25.0,
  "frames": [
    { "tick": 0, "timestamp_ns": 0, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[-100.0, 0.0, 0.0], [100.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[1900.0, 0.0, 0.0], [2100.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 1, "timestamp_ns": 1000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[-100.0, 0.0, 0.0], [100.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[900.0, 0.0, 0.0], [1100.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 2, "timestamp_ns": 2000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[-100.0, 0.0, 0.0], [100.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [null, null],
         "keypoints_valid": [false, false], "reprojection_error_px": [null, null], "reprojected_px": {}}
    ]},
    { "tick": 3, "timestamp_ns": 3000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[-100.0, 0.0, 0.0], [100.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[400.0, 0.0, 0.0], [600.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 4, "timestamp_ns": 4000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[-100.0, 0.0, 0.0], [100.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[2400.0, 0.0, 0.0], [2600.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]}
  ]
}
)JSON";

// keypoint_names = [left_hip, right_hip] only. 6 ticks, 1s apart, 4 tracks:
// track0/track1's hip-midpoint X positions make track1's per-interval speed
// EXACTLY 2x track0's every interval (a pure positive linear relationship,
// B=2A -> Pearson r=+1 exactly for any window of >=2 points on it).
// track2/track3 are built so track3's speed = 600 - track2's speed every
// interval (a pure negative linear relationship -> r=-1 exactly).
const char* kCongruentMotionFixtureJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1",
  "source_videos": [], "cameras": [],
  "keypoint_names": ["left_hip", "right_hip"],
  "skeleton_edges": [], "master_fps": 25.0,
  "frames": [
    { "tick": 0, "timestamp_ns": 0, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[-100.0, 0.0, 0.0], [100.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[-100.0, 500.0, 0.0], [100.0, 500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 2, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[-100.0, 1000.0, 0.0], [100.0, 1000.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 3, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[-100.0, 1500.0, 0.0], [100.0, 1500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 1, "timestamp_ns": 1000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[0.0, 0.0, 0.0], [200.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[100.0, 500.0, 0.0], [300.0, 500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 2, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[0.0, 1000.0, 0.0], [200.0, 1000.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 3, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[400.0, 1500.0, 0.0], [600.0, 1500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 2, "timestamp_ns": 2000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[200.0, 0.0, 0.0], [400.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[500.0, 500.0, 0.0], [700.0, 500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 2, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[200.0, 1000.0, 0.0], [400.0, 1000.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 3, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[800.0, 1500.0, 0.0], [1000.0, 1500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 3, "timestamp_ns": 3000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[500.0, 0.0, 0.0], [700.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[1100.0, 500.0, 0.0], [1300.0, 500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 2, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[500.0, 1000.0, 0.0], [700.0, 1000.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 3, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[1100.0, 1500.0, 0.0], [1300.0, 1500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 4, "timestamp_ns": 4000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[900.0, 0.0, 0.0], [1100.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[1900.0, 500.0, 0.0], [2100.0, 500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 2, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[900.0, 1000.0, 0.0], [1100.0, 1000.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 3, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[1300.0, 1500.0, 0.0], [1500.0, 1500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 5, "timestamp_ns": 5000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[1400.0, 0.0, 0.0], [1600.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[2900.0, 500.0, 0.0], [3100.0, 500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 2, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[1400.0, 1000.0, 0.0], [1600.0, 1000.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 3, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[1400.0, 1500.0, 0.0], [1600.0, 1500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]}
  ]
}
)JSON";

// Regression fixture for a real bug caught by code review: track 0 (A) is
// present and valid at EVERY tick (0-5); track 1 (B) is present at ticks
// 0,1,2,4,5 but ENTIRELY ABSENT from tick 3's "people" array (not just an
// invalid keypoint while still listed — a distinct code path from
// kApproachRateFixtureJson's gap, which never exercises this one). A's own
// speed must be derived from A's own continuous presence, independent of
// B's absence — the bug this catches: an earlier version of
// compute_dyadic_kinematics() gated BOTH people's hip-midpoint capture on
// BOTH being present that tick, so B's absence at tick 3 also silently
// dropped A's perfectly-valid tick-3 sample, corrupting A's own speed
// derivation across ticks 2-4 into one wide (and wrong) average.
//
// A's hip-midpoint x: [0, 100, 200, 210, 400, 410] at t=[0..5]s ->
// per-interval speed [100, 100, 10, 190, 10] IF each interval is correctly
// measured 1 tick apart (requires A's tick-3 sample to survive).
// B's hip-midpoint x (present at 0,1,2,4,5 only): [0, 200, 400, -, 1160, 1180]
// -> per-interval speed [200, 200, 380 (over the real 2s gap across B's own
// absent tick 3), 20] — chosen as EXACTLY 2x A's correctly-derived speed at
// every one of B's own valid intervals, so a correct implementation gives
// congruentMotionCorr == +1.0 exactly once >=3 paired samples exist; the
// buggy implementation instead computes A's speed at tick 4 as
// (400-200)/2s = 100 (not 190), breaking the exact 2x relationship and
// producing a correlation measurably less than 1.0.
const char* kIndependentPresenceFixtureJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1",
  "source_videos": [], "cameras": [],
  "keypoint_names": ["left_hip", "right_hip"],
  "skeleton_edges": [], "master_fps": 25.0,
  "frames": [
    { "tick": 0, "timestamp_ns": 0, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[-100.0, 0.0, 0.0], [100.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[-100.0, 500.0, 0.0], [100.0, 500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 1, "timestamp_ns": 1000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[0.0, 0.0, 0.0], [200.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[100.0, 500.0, 0.0], [300.0, 500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 2, "timestamp_ns": 2000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[100.0, 0.0, 0.0], [300.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[300.0, 500.0, 0.0], [500.0, 500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 3, "timestamp_ns": 3000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[110.0, 0.0, 0.0], [310.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 4, "timestamp_ns": 4000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[300.0, 0.0, 0.0], [500.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[1060.0, 500.0, 0.0], [1260.0, 500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]},
    { "tick": 5, "timestamp_ns": 5000000000, "people": [
        {"track_id": 0, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[310.0, 0.0, 0.0], [510.0, 0.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}},
        {"track_id": 1, "num_contributing_cameras": 0, "source_cameras": [],
         "keypoints_room": [[1080.0, 500.0, 0.0], [1280.0, 500.0, 0.0]],
         "keypoints_valid": [true, true], "reprojection_error_px": [0, 0], "reprojected_px": {}}
    ]}
  ]
}
)JSON";

Skeleton3DResult load_fixture(const QTemporaryDir& dir, const char* json, const char* name) {
    const QString path = dir.path() + "/" + name;
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(json);
    f.close();
    return Skeleton3DResult::load(path);
}

} // namespace

TEST(DyadicKinematics, FacingCosineFaceToFaceIsExactlyNegativeOne) {
    QTemporaryDir dir;
    const auto result = load_fixture(dir, kFacingFixtureJson, "facing.json");
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 1);
    ASSERT_EQ(series.samples.size(), 1);
    EXPECT_NEAR(series.samples[0].distanceMm, 1000.0, 1e-6);
    EXPECT_NEAR(series.samples[0].facingCosine, -1.0, 1e-9);
    EXPECT_NEAR(series.stats.meanDistanceMm, 1000.0, 1e-6);
}

TEST(DyadicKinematics, FacingCosineSideBySideIsExactlyPositiveOne) {
    QTemporaryDir dir;
    const auto result = load_fixture(dir, kFacingFixtureJson, "facing.json");
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 2);
    ASSERT_EQ(series.samples.size(), 1);
    EXPECT_NEAR(series.samples[0].distanceMm, 300.0, 1e-6);
    EXPECT_NEAR(series.samples[0].facingCosine, 1.0, 1e-9);
}

TEST(DyadicKinematics, FacingCosinePerpendicularIsExactlyZero) {
    QTemporaryDir dir;
    const auto result = load_fixture(dir, kFacingFixtureJson, "facing.json");
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 3);
    ASSERT_EQ(series.samples.size(), 1);
    EXPECT_NEAR(series.samples[0].distanceMm, 1000.0, 1e-6);
    EXPECT_NEAR(series.samples[0].facingCosine, 0.0, 1e-9);
}

TEST(DyadicKinematics, ApproachRateUsesRealElapsedTimeAcrossAGap) {
    QTemporaryDir dir;
    const auto result = load_fixture(dir, kApproachRateFixtureJson, "approach.json");
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 1);
    ASSERT_EQ(series.samples.size(), 5);

    EXPECT_NEAR(series.samples[0].distanceMm, 2000.0, 1e-6);
    EXPECT_TRUE(std::isnan(series.samples[0].approachRateMmPerS)); // first valid sample, no prior

    EXPECT_NEAR(series.samples[1].distanceMm, 1000.0, 1e-6);
    EXPECT_NEAR(series.samples[1].approachRateMmPerS, -1000.0, 1e-6);

    EXPECT_TRUE(std::isnan(series.samples[2].distanceMm)); // B's hips invalid this tick

    // Real 2s gap (tick1 -> tick3, tick2 skipped), not a 1-tick assumption.
    EXPECT_NEAR(series.samples[3].distanceMm, 500.0, 1e-6);
    EXPECT_NEAR(series.samples[3].approachRateMmPerS, -250.0, 1e-6);

    EXPECT_NEAR(series.samples[4].distanceMm, 2500.0, 1e-6);
    EXPECT_NEAR(series.samples[4].approachRateMmPerS, 2000.0, 1e-6); // retreating -> positive

    EXPECT_NEAR(series.stats.pctTicksBothPresent, 4.0 / 5.0, 1e-9);
}

TEST(DyadicKinematics, MissingFacingKeypointNamesLeavesFacingCosineAlwaysNan) {
    QTemporaryDir dir;
    const auto result = load_fixture(dir, kApproachRateFixtureJson, "approach.json");
    ASSERT_TRUE(result.is_valid());
    ASSERT_EQ(result.keypoint_names().indexOf("nose"), -1); // fixture has no facing keypoints

    const auto series = compute_dyadic_kinematics(result, 0, 1);
    for (const auto& sample : series.samples) {
        EXPECT_TRUE(std::isnan(sample.facingCosine));
    }
    EXPECT_TRUE(std::isnan(series.stats.meanFacingCosine));
    // Distance still computes fine despite the missing facing keypoints —
    // graceful degradation, not a crash.
    EXPECT_NEAR(series.samples[0].distanceMm, 2000.0, 1e-6);
}

TEST(DyadicKinematics, CongruentMotionPerfectPositiveCorrelationIsExactlyOne) {
    QTemporaryDir dir;
    const auto result = load_fixture(dir, kCongruentMotionFixtureJson, "congruent.json");
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 1, /*congruentMotionWindow=*/30);
    ASSERT_EQ(series.samples.size(), 6);

    // First two paired speed samples (ticks 1,2) give a window of <3 ->
    // undefined correlation, matching the documented minimum.
    EXPECT_TRUE(std::isnan(series.samples[1].congruentMotionCorr));
    EXPECT_TRUE(std::isnan(series.samples[2].congruentMotionCorr));
    // From the 3rd paired sample onward, every window is drawn from an
    // exact B=2*A linear relationship, so correlation is exactly +1
    // regardless of window size.
    EXPECT_NEAR(series.samples[3].congruentMotionCorr, 1.0, 1e-9);
    EXPECT_NEAR(series.samples[4].congruentMotionCorr, 1.0, 1e-9);
    EXPECT_NEAR(series.samples[5].congruentMotionCorr, 1.0, 1e-9);
    EXPECT_NEAR(series.stats.meanCongruentMotionCorr, 1.0, 1e-9);
}

TEST(DyadicKinematics, CongruentMotionPerfectNegativeCorrelationIsExactlyNegativeOne) {
    QTemporaryDir dir;
    const auto result = load_fixture(dir, kCongruentMotionFixtureJson, "congruent.json");
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 2, 3, /*congruentMotionWindow=*/30);
    ASSERT_EQ(series.samples.size(), 6);

    EXPECT_TRUE(std::isnan(series.samples[1].congruentMotionCorr));
    EXPECT_NEAR(series.samples[3].congruentMotionCorr, -1.0, 1e-9);
    EXPECT_NEAR(series.samples[5].congruentMotionCorr, -1.0, 1e-9);
}

TEST(DyadicKinematics, InvalidResultOrSameTrackIdReturnsEmptySeries) {
    // A default-constructed (never load()'d) result is invalid.
    const Skeleton3DResult invalid;
    const auto emptyFromInvalid = compute_dyadic_kinematics(invalid, 0, 1);
    EXPECT_TRUE(emptyFromInvalid.samples.isEmpty());

    QTemporaryDir dir;
    const auto result = load_fixture(dir, kFacingFixtureJson, "facing.json");
    ASSERT_TRUE(result.is_valid());

    const auto sameTrack = compute_dyadic_kinematics(result, 0, 0);
    EXPECT_TRUE(sameTrack.samples.isEmpty());
}

TEST(DyadicKinematics, OnePersonsSpeedIsUnaffectedByTheOtherPersonsAbsence) {
    QTemporaryDir dir;
    const auto result =
        load_fixture(dir, kIndependentPresenceFixtureJson, "independent_presence.json");
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 0, 1, /*congruentMotionWindow=*/30);
    ASSERT_EQ(series.samples.size(), 6);

    // Tick 3: B is entirely absent -> distance/facing NaN, as expected —
    // this alone doesn't prove the bug is fixed, the correlation values
    // below do.
    EXPECT_TRUE(std::isnan(series.samples[3].distanceMm));

    // pairedIdxs = {1, 2, 4, 5} (B has no speed sample at 3 either way,
    // since B is genuinely absent there). count>=3 first at index 4 (the
    // 3rd paired sample) — if A's own tick-3 sample survived B's absence
    // (the fix), speedA at every paired index is exactly half speedB,
    // giving r=+1.0 exactly. If it didn't (the bug), A's speed at index 4
    // is computed across a spurious 2-tick gap instead of 1, breaking the
    // exact proportionality.
    EXPECT_TRUE(std::isnan(series.samples[1].congruentMotionCorr));
    EXPECT_TRUE(std::isnan(series.samples[2].congruentMotionCorr));
    EXPECT_NEAR(series.samples[4].congruentMotionCorr, 1.0, 1e-9);
    EXPECT_NEAR(series.samples[5].congruentMotionCorr, 1.0, 1e-9);
}
