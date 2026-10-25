#include <gtest/gtest.h>

#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <array>
#include <cmath>
#include <numbers>

#include "analysis/dyadic_kinematics.hpp"
#include "analysis/skeleton3d_result.hpp"

using mosaic::compute_dyadic_kinematics;
using mosaic::compute_person_kinematics;
using mosaic::Skeleton3DResult;

namespace {

// The reader numbers people from 1: a result without person_id (all these
// fixtures) gives person track_id + 1, so track 0 below is person 1.

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

    const auto series = compute_dyadic_kinematics(result, 1, 2);
    ASSERT_EQ(series.samples.size(), 1);
    EXPECT_NEAR(series.samples[0].distanceMm, 1000.0, 1e-6);
    EXPECT_NEAR(series.samples[0].facingCosine, -1.0, 1e-9);
    EXPECT_NEAR(series.stats.meanDistanceMm, 1000.0, 1e-6);
}

TEST(DyadicKinematics, FacingCosineSideBySideIsExactlyPositiveOne) {
    QTemporaryDir dir;
    const auto result = load_fixture(dir, kFacingFixtureJson, "facing.json");
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 1, 3);
    ASSERT_EQ(series.samples.size(), 1);
    EXPECT_NEAR(series.samples[0].distanceMm, 300.0, 1e-6);
    EXPECT_NEAR(series.samples[0].facingCosine, 1.0, 1e-9);
}

TEST(DyadicKinematics, FacingCosinePerpendicularIsExactlyZero) {
    QTemporaryDir dir;
    const auto result = load_fixture(dir, kFacingFixtureJson, "facing.json");
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 1, 4);
    ASSERT_EQ(series.samples.size(), 1);
    EXPECT_NEAR(series.samples[0].distanceMm, 1000.0, 1e-6);
    EXPECT_NEAR(series.samples[0].facingCosine, 0.0, 1e-9);
}

TEST(DyadicKinematics, ApproachRateUsesRealElapsedTimeAcrossAGap) {
    QTemporaryDir dir;
    const auto result = load_fixture(dir, kApproachRateFixtureJson, "approach.json");
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 1, 2);
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

    const auto series = compute_dyadic_kinematics(result, 1, 2);
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

    const auto series = compute_dyadic_kinematics(result, 1, 2, /*congruentMotionWindow=*/30);
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

    const auto series = compute_dyadic_kinematics(result, 3, 4, /*congruentMotionWindow=*/30);
    ASSERT_EQ(series.samples.size(), 6);

    EXPECT_TRUE(std::isnan(series.samples[1].congruentMotionCorr));
    EXPECT_NEAR(series.samples[3].congruentMotionCorr, -1.0, 1e-9);
    EXPECT_NEAR(series.samples[5].congruentMotionCorr, -1.0, 1e-9);
}

TEST(DyadicKinematics, InvalidResultOrSameTrackIdReturnsEmptySeries) {
    // A default-constructed (never load()'d) result is invalid.
    const Skeleton3DResult invalid;
    const auto emptyFromInvalid = compute_dyadic_kinematics(invalid, 1, 2);
    EXPECT_TRUE(emptyFromInvalid.samples.isEmpty());

    QTemporaryDir dir;
    const auto result = load_fixture(dir, kFacingFixtureJson, "facing.json");
    ASSERT_TRUE(result.is_valid());

    const auto sameTrack = compute_dyadic_kinematics(result, 1, 1);
    EXPECT_TRUE(sameTrack.samples.isEmpty());
}

TEST(DyadicKinematics, OnePersonsSpeedIsUnaffectedByTheOtherPersonsAbsence) {
    QTemporaryDir dir;
    const auto result =
        load_fixture(dir, kIndependentPresenceFixtureJson, "independent_presence.json");
    ASSERT_TRUE(result.is_valid());

    const auto series = compute_dyadic_kinematics(result, 1, 2, /*congruentMotionWindow=*/30);
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

// ── One person's own series ──────────────────────────────────────────────

TEST(PersonKinematics, SpeedAndHeightsOfOnePerson) {
    // Up is -y (a level reference camera). Person 1 stands with the head
    // 1.6 m and the hips 0.9 m above ankles at 0.08 m (floor at 0), and
    // walks along x at 1 m/s, 10 ticks per second; at tick 5 it is not in
    // 3D.
    QString frames;
    for (int k = 0; k < 10; ++k) {
        const double x = 100.0 * k;
        // Built from one-line strings: clang-format lays a multi-line raw
        // string out differently with CRLF and LF line endings.
        QString person;
        if (k != 5) {
            person = QString(R"({"track_id": 0, "num_contributing_cameras": 3, )"
                             R"("keypoints_room": [[%1,-1600,3000],[%1,-900,3000],[%2,-900,3000],)"
                             R"([%1,-80,3000],[%2,-80,3000]], )"
                             R"("keypoints_valid": [true,true,true,true,true]})")
                         .arg(x)
                         .arg(x + 1.0);
        }
        frames += QString(R"(%1{"tick": %2, "timestamp_ns": %3, "people": [%4]})")
                      .arg(k ? "," : "")
                      .arg(k)
                      .arg(100000000LL * k)
                      .arg(person);
    }
    const QString json =
        QString(
            R"({"schema": "mosaic-skeleton3d-v1", "cameras": [], )"
            R"("keypoint_names": ["nose", "left_hip", "right_hip", "left_ankle", "right_ankle"], )"
            R"("skeleton_edges": [], "frames": [%1]})")
            .arg(frames);
    QTemporaryDir dir;
    QFile f(dir.filePath("s.json"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(json.toUtf8());
    f.close();
    const auto result = Skeleton3DResult::load(dir.filePath("s.json"));
    ASSERT_TRUE(result.is_valid());

    const auto s = compute_person_kinematics(result, 1);
    ASSERT_EQ(s.samples.size(), 10);
    EXPECT_NEAR(s.samples[2].speedMmPerS, 1000.0, 1e-6);
    EXPECT_NEAR(s.samples[2].headHeightMm, 1600.0, 1e-6);
    EXPECT_NEAR(s.samples[2].hipHeightMm, 900.0, 1e-6);
    EXPECT_EQ(s.samples[2].cameras, 3);
    // Not in 3D at tick 5: nothing there, and no speed across the gap.
    EXPECT_EQ(s.samples[5].cameras, 0);
    EXPECT_TRUE(std::isnan(s.samples[5].hipHeightMm));
    EXPECT_TRUE(std::isnan(s.samples[4].speedMmPerS));
    EXPECT_TRUE(std::isnan(s.samples[6].speedMmPerS));
    EXPECT_NEAR(s.stats.secondsSeen, 0.9, 1e-9);
    EXPECT_NEAR(s.stats.medianHeadHeightMm, 1600.0, 1e-6);
    // Hips 0.9 m up, walking at 1 m/s: walking all 0.9 s in 3D.
    EXPECT_NEAR(s.stats.secondsByPosture[static_cast<int>(mosaic::Posture::Walking)], 0.9, 1e-9);
    // Nobody with another number.
    EXPECT_EQ(compute_person_kinematics(result, 2).samples[0].cameras, 0);
}

TEST(PersonKinematics, JointAnglesAreFlexionFromStandingStraight) {
    // Forward is -z, up -y. Tick 0: upright, the left leg bent as sitting
    // (thigh forward, shank down), the right leg straight, the left arm
    // hanging, the right arm straight out sideways with the forearm pointing
    // up. Tick 1: the same, the shoulders 30 degrees forward over the hips.
    // Tick 2: as tick 0 without the right wrist. Tick 3: without the left
    // hip. The raw keypoints have the left leg straight: the angles must
    // come from the smoothed ones.
    using P            = std::array<double, 3>;
    const double s30   = 500.0 * std::sin(std::numbers::pi / 6);
    const double c30   = 500.0 * std::cos(std::numbers::pi / 6);
    const auto pose_at = [&](bool lean, int missing) {
        // Shoulders 500 mm above the hips; leaning moves them forward.
        const P sh = lean ? P{0, -800 - c30, 3000 - s30} : P{0, -1300, 3000};
        const QVector<P> kp{
            {sh[0], sh[1] - 250, sh[2]},       // nose
            {sh[0] - 200, sh[1], sh[2]},       // left_shoulder
            {sh[0] + 200, sh[1], sh[2]},       // right_shoulder
            {sh[0] - 200, sh[1] + 300, sh[2]}, // left_elbow, below the shoulder
            {sh[0] + 500, sh[1], sh[2]},       // right_elbow, out to the side
            {sh[0] - 200, sh[1] + 600, sh[2]}, // left_wrist, below the elbow
            {sh[0] + 500, sh[1] - 300, sh[2]}, // right_wrist, above the elbow
            {-150, -800, 3000},                // left_hip
            {150, -800, 3000},                 // right_hip
            {-150, -800, 2600},                // left_knee, forward
            {150, -440, 3000},                 // right_knee, below
            {-150, -80, 2600},                 // left_ankle, below the knee
            {150, -80, 3000},                  // right_ankle
        };
        const auto at = [](const P& p) {
            return QString("[%1,%2,%3]")
                .arg(p[0], 0, 'f', 6)
                .arg(p[1], 0, 'f', 6)
                .arg(p[2], 0, 'f', 6);
        };
        QStringList raw, smoothed, valid;
        for (int i = 0; i < kp.size(); ++i) {
            smoothed << at(kp[i]);
            // Raw: the left knee and ankle straight below the hip.
            raw << (i == 9 ? at({-150, -440, 3000}) : i == 11 ? at({-150, -80, 3000}) : at(kp[i]));
            valid << (i == missing ? "false" : "true");
        }
        return QString(R"({"track_id": 0, "num_contributing_cameras": 2, )"
                       R"("keypoints_room": [%1], "keypoints_room_smoothed": [%2], )"
                       R"("keypoints_valid": [%3]})")
            .arg(raw.join(","), smoothed.join(","), valid.join(","));
    };
    constexpr int kRightWrist = 6;
    constexpr int kLeftHip    = 7;
    const QString frames      = QString(R"({"tick": 0, "timestamp_ns": 0, "people": [%1]},)"
                                             R"({"tick": 1, "timestamp_ns": 100000000, "people": [%2]},)"
                                             R"({"tick": 2, "timestamp_ns": 200000000, "people": [%3]},)"
                                             R"({"tick": 3, "timestamp_ns": 300000000, "people": [%4]})")
                               .arg(pose_at(false, -1), pose_at(true, -1),
                                    pose_at(false, kRightWrist), pose_at(false, kLeftHip));
    const QString json =
        QString(R"({"schema": "mosaic-skeleton3d-v1", "cameras": [], "keypoint_names": [)"
                R"("nose","left_shoulder","right_shoulder","left_elbow","right_elbow",)"
                R"("left_wrist","right_wrist","left_hip","right_hip","left_knee","right_knee",)"
                R"("left_ankle","right_ankle"], "skeleton_edges": [], "frames": [%1]})")
            .arg(frames);
    QTemporaryDir dir;
    QFile f(dir.filePath("s.json"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(json.toUtf8());
    f.close();
    const auto result = Skeleton3DResult::load(dir.filePath("s.json"));
    ASSERT_TRUE(result.is_valid());

    const auto s = compute_person_kinematics(result, 1);
    ASSERT_EQ(s.samples.size(), 4);
    const auto& up = s.samples[0];
    EXPECT_NEAR(up.kneeFlexionDeg.left, 90.0, 1e-6);
    EXPECT_NEAR(up.kneeFlexionDeg.right, 0.0, 1e-6);
    EXPECT_NEAR(up.hipFlexionDeg.left, 90.0, 1e-6);
    EXPECT_NEAR(up.hipFlexionDeg.right, 0.0, 1e-6);
    EXPECT_NEAR(up.armRaiseDeg.left, 0.0, 1e-6);
    EXPECT_NEAR(up.armRaiseDeg.right, 90.0, 1e-6);
    EXPECT_NEAR(up.elbowFlexionDeg.left, 0.0, 1e-6);
    EXPECT_NEAR(up.elbowFlexionDeg.right, 90.0, 1e-6);
    // Leaning 30 degrees forward over a thigh at 90: the hip is at 120.
    EXPECT_NEAR(s.samples[1].hipFlexionDeg.left, 120.0, 1e-6);
    // Without the right wrist, no right elbow; the rest is unaffected.
    EXPECT_TRUE(std::isnan(s.samples[2].elbowFlexionDeg.right));
    EXPECT_NEAR(s.samples[2].elbowFlexionDeg.left, 0.0, 1e-6);
    EXPECT_NEAR(s.samples[2].armRaiseDeg.right, 90.0, 1e-6);
    // Without the left hip there is no trunk: no hip flexion or arm raise on
    // either side, and no left knee; the right knee and the elbows stay.
    const auto& noHip = s.samples[3];
    EXPECT_TRUE(std::isnan(noHip.hipFlexionDeg.left));
    EXPECT_TRUE(std::isnan(noHip.hipFlexionDeg.right));
    EXPECT_TRUE(std::isnan(noHip.armRaiseDeg.right));
    EXPECT_TRUE(std::isnan(noHip.kneeFlexionDeg.left));
    EXPECT_NEAR(noHip.kneeFlexionDeg.right, 0.0, 1e-6);
    EXPECT_NEAR(noHip.elbowFlexionDeg.right, 90.0, 1e-6);
    // The usual angles, over the ticks each is known: hip left 90, 120, 90.
    EXPECT_NEAR(s.stats.medianHipFlexionDeg.left, 90.0, 1e-6);
    EXPECT_NEAR(s.stats.medianKneeFlexionDeg.left, 90.0, 1e-6);
    EXPECT_NEAR(s.stats.medianElbowFlexionDeg.right, 90.0, 1e-6);
    EXPECT_NEAR(s.stats.medianArmRaiseDeg.left, 0.0, 1e-6);
}

TEST(PersonKinematics, MovementRatesOverNeighbouringTicks) {
    // Ticks 0.1 s apart, then one after a gap (at 1 s). Still, except: the
    // left wrist moves down at 2 m/s, along the forearm, so the elbow stays
    // straight; the left shank swings forward at 100 deg/s about the knee;
    // the ears turn about the vertical at 200 deg/s around a still head.
    using P            = std::array<double, 3>;
    const double rad   = std::numbers::pi / 180.0;
    const auto pose_at = [&](int k) {
        const double th = 10.0 * k * rad; // knee flexion
        const double ph = 20.0 * k * rad; // head turn
        const QVector<P> kp{
            {-200, -1300, 3000},                                          // left_shoulder
            {200, -1300, 3000},                                           // right_shoulder
            {-200, -1000, 3000},                                          // left_elbow
            {200, -1000, 3000},                                           // right_elbow
            {-200, -700.0 + 200.0 * k, 3000},                             // left_wrist
            {200, -700, 3000},                                            // right_wrist
            {-150, -800, 3000},                                           // left_hip
            {150, -800, 3000},                                            // right_hip
            {-150, -440, 3000},                                           // left_knee
            {150, -440, 3000},                                            // right_knee
            {-150, -440 + 360 * std::cos(th), 3000 - 360 * std::sin(th)}, // left_ankle
            {150, -80, 3000},                                             // right_ankle
            {-75 * std::cos(ph), -1450, 3000 - 75 * std::sin(ph)},        // left_ear
            {75 * std::cos(ph), -1450, 3000 + 75 * std::sin(ph)},         // right_ear
        };
        QStringList pts, valid;
        for (const auto& p : kp) {
            pts << QString("[%1,%2,%3]")
                       .arg(p[0], 0, 'f', 6)
                       .arg(p[1], 0, 'f', 6)
                       .arg(p[2], 0, 'f', 6);
            valid << "true";
        }
        return QString(R"({"track_id": 0, "num_contributing_cameras": 2, )"
                       R"("keypoints_room": [%1], "keypoints_valid": [%2]})")
            .arg(pts.join(","), valid.join(","));
    };
    QStringList frames;
    for (int k = 0; k < 6; ++k) {
        const int64_t t = k < 5 ? 100000000LL * k : 1000000000LL;
        frames << QString(R"({"tick": %1, "timestamp_ns": %2, "people": [%3]})")
                      .arg(k)
                      .arg(t)
                      .arg(pose_at(k));
    }
    const QString json =
        QString(R"({"schema": "mosaic-skeleton3d-v1", "cameras": [], "keypoint_names": [)"
                R"("left_shoulder","right_shoulder","left_elbow","right_elbow","left_wrist",)"
                R"("right_wrist","left_hip","right_hip","left_knee","right_knee","left_ankle",)"
                R"("right_ankle","left_ear","right_ear"], "skeleton_edges": [], "frames": [%1]})")
            .arg(frames.join(","));
    QTemporaryDir dir;
    QFile f(dir.filePath("s.json"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(json.toUtf8());
    f.close();
    const auto result = Skeleton3DResult::load(dir.filePath("s.json"));
    ASSERT_TRUE(result.is_valid());

    const auto s = compute_person_kinematics(result, 1);
    ASSERT_EQ(s.samples.size(), 6);
    for (int k : {1, 2, 3}) {
        const auto& m = s.samples[k];
        EXPECT_NEAR(m.wristSpeedMmPerS.left, 2000.0, 1e-6) << k;
        EXPECT_NEAR(m.wristSpeedMmPerS.right, 0.0, 1e-6) << k;
        EXPECT_NEAR(m.kneeFlexionRateDegPerS.left, 100.0, 1e-6) << k;
        EXPECT_NEAR(m.kneeFlexionRateDegPerS.right, 0.0, 1e-6) << k;
        EXPECT_NEAR(m.elbowFlexionRateDegPerS.left, 0.0, 1e-6) << k;
        EXPECT_NEAR(m.headSpeedMmPerS, 0.0, 1e-6) << k;
        EXPECT_NEAR(m.headTurnDegPerS, 200.0, 1e-4) << k; // 6-decimal positions
        EXPECT_NEAR(m.speedMmPerS, 0.0, 1e-6) << k;
        // The mean over the 14 keypoints: the wrist, the ankle's chord over
        // 20 deg on 360 mm and each ear's over 40 deg on 75 mm, in 0.2 s.
        const double moved =
            400.0 + 2 * 360 * std::sin(10 * rad) + 2 * (2 * 75 * std::sin(20 * rad));
        EXPECT_NEAR(m.motionMmPerS, moved / 0.2 / 14, 1e-6) << k;
    }
    // Not across the gap (tick 4's next neighbour is 0.7 s away), nor at the
    // ends.
    for (int k : {0, 4, 5}) {
        EXPECT_TRUE(std::isnan(s.samples[k].wristSpeedMmPerS.left)) << k;
        EXPECT_TRUE(std::isnan(s.samples[k].kneeFlexionRateDegPerS.left)) << k;
        EXPECT_TRUE(std::isnan(s.samples[k].motionMmPerS)) << k;
        EXPECT_TRUE(std::isnan(s.samples[k].headTurnDegPerS)) << k;
    }
}

TEST(PersonKinematics, SpreadIsTheMedianAndNearestRank95thPercentile) {
    QVector<mosaic::PersonSample> samples(25);
    for (int i = 0; i < 20; ++i) {
        samples[i].motionMmPerS = 20 - i; // 20 down to 1; the last 5 unknown
    }
    const auto sp =
        mosaic::spread_of(samples, [](const mosaic::PersonSample& s) { return s.motionMmPerS; });
    EXPECT_DOUBLE_EQ(sp.median, 11.0); // the upper middle of 20
    EXPECT_DOUBLE_EQ(sp.p95, 19.0);    // the 19th of 20
    const auto none =
        mosaic::spread_of({}, [](const mosaic::PersonSample& s) { return s.motionMmPerS; });
    EXPECT_TRUE(std::isnan(none.median) && std::isnan(none.p95));
}

TEST(PersonKinematics, PostureFromTheHipsHeightAndSpeed) {
    using mosaic::PersonSample;
    using mosaic::Posture;
    // 25 ticks a second. 3 s sitting (hips 0.65 m), 3 s standing (0.95 m,
    // a speed spike every 5th tick), 3 s walking at 0.8 m/s with 0.2 s of
    // standing in it, then 2 s not in 3D and 0.5 s standing alone. While
    // sitting, the hips are not seen for 3 ticks.
    QVector<PersonSample> s;
    const auto add = [&](double hipMm, double speedMmPerS, int cameras = 2) {
        PersonSample p;
        p.timestampNs = 40000000LL * s.size();
        p.hipHeightMm = hipMm;
        p.speedMmPerS = speedMmPerS;
        p.cameras     = cameras;
        s << p;
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (int k = 0; k < 75; ++k) {
        add(k >= 30 && k < 33 ? nan : 650.0, 50.0);
    }
    for (int k = 0; k < 75; ++k) {
        add(950.0, k % 5 == 0 ? 2000.0 : 100.0);
    }
    for (int k = 0; k < 75; ++k) {
        add(950.0, k >= 30 && k < 35 ? 100.0 : 800.0);
    }
    for (int k = 0; k < 50; ++k) {
        add(nan, nan, 0);
    }
    for (int k = 0; k < 12; ++k) {
        add(950.0, 100.0);
    }
    const auto p = mosaic::classify_posture(s);
    ASSERT_EQ(p.size(), s.size());
    // Sitting, but not the 3 ticks without hips: never filled in.
    for (int k = 0; k < 75; ++k) {
        EXPECT_EQ(p[k], k >= 30 && k < 33 ? Posture::Unknown : Posture::Sitting) << k;
    }
    // Standing: a spike every 5th tick does not move the median. Within
    // half a second of walking, the window holds more walking than standing,
    // so the change can come up to that much early.
    for (int k = 75; k < 144; ++k) {
        EXPECT_EQ(p[k], Posture::Standing) << k;
    }
    // Walking, the 0.2 s pause included; near the standing run its window
    // mixes both, and the median decides within half a second.
    for (int k = 150; k < 225; ++k) {
        EXPECT_EQ(p[k], Posture::Walking) << k;
    }
    // Not in 3D: unknown; the short standing run after the gap has nothing
    // else in its window, so it stays.
    for (int k = 225; k < 275; ++k) {
        EXPECT_EQ(p[k], Posture::Unknown) << k;
    }
    for (int k = 275; k < 287; ++k) {
        EXPECT_EQ(p[k], Posture::Standing) << k;
    }

    // Standing broken up by ticks without hips stays standing: no gap in
    // the hips turns it into the posture before (sub-39's last seconds).
    QVector<PersonSample> broken;
    for (int k = 0; k < 100; ++k) {
        PersonSample q;
        q.timestampNs = 40000000LL * k;
        q.cameras     = 2;
        q.hipHeightMm = k < 40 ? 650.0 : (k % 7 < 3 ? nan : 950.0);
        q.speedMmPerS = 100.0;
        broken << q;
    }
    const auto b = mosaic::classify_posture(broken);
    for (int k = 55; k < 100; ++k) {
        EXPECT_EQ(b[k], broken[k].hipHeightMm > 0 ? Posture::Standing : Posture::Unknown) << k;
    }
    // Right after sitting, the window still holds more sitting than the few
    // standing ticks: the mode filter makes the first two sitting.
    EXPECT_EQ(b[40], Posture::Sitting);
    EXPECT_EQ(b[41], Posture::Sitting);

    // The mode filter: standing, with the hips read low one tick in six.
    // Each low tick alone is sitting; among standing ticks it is standing.
    QVector<PersonSample> dips;
    for (int k = 0; k < 60; ++k) {
        PersonSample q;
        q.timestampNs = 40000000LL * k;
        q.cameras     = 2;
        q.hipHeightMm = k % 6 == 3 ? 750.0 : 950.0;
        q.speedMmPerS = 100.0;
        dips << q;
    }
    const auto d = mosaic::classify_posture(dips);
    for (int k = 0; k < 60; ++k) {
        EXPECT_EQ(d[k], Posture::Standing) << k;
    }
}
