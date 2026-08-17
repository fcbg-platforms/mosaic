#include "analysis/skeleton3d_result.hpp"
#include <gtest/gtest.h>
#include <QFile>
#include <QTemporaryDir>

using mosaic::Skeleton3DResult;

namespace {

// Matches the exact schema written by analysis/run_pose3d.py's
// _write_results(). Tick 0 has 2 people (one with a fully-valid keypoint
// set, one with a partially-invalid keypoint at index 1) spanning 2/3
// cameras; tick 1 has 1 person; tick 2 has 0 people — covering every
// branch of the schema.
const char* kFixtureJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1",
  "source_videos": ["video/video_0.mp4", "video/video_1.mp4", "video/video_2.mp4"],
  "cameras": [
    {"index": 0, "position_room": [0.0, 0.0, 0.0]},
    {"index": 1, "position_room": [500.0, 0.0, 100.0]},
    {"index": 2, "position_room": [0.0, 500.0, -100.0]}
  ],
  "keypoint_names": ["nose", "left_eye", "right_eye"],
  "skeleton_edges": [[0, 1], [0, 2]],
  "master_fps": 25.0,
  "params": {"min_cameras": 2, "max_reprojection_error_px": 15.0},
  "frames": [
    {
      "tick": 0, "timestamp_ns": 1000000000,
      "people": [
        {
          "track_id": 0, "num_contributing_cameras": 3, "source_cameras": [0, 1, 2],
          "keypoints_room": [[10.0, 20.0, 900.0], [12.0, 25.0, 895.0], [8.0, 25.0, 895.0]],
          "keypoints_valid": [true, true, true],
          "reprojection_error_px": [2.1, 1.8, 2.4],
          "reprojected_px": {
            "0": [[500.0, 300.0], [505.0, 295.0], [495.0, 295.0]],
            "1": [[480.0, 310.0], [485.0, 305.0], [475.0, 305.0]],
            "2": [[520.0, 290.0], [525.0, 285.0], [515.0, 285.0]]
          }
        },
        {
          "track_id": 1, "num_contributing_cameras": 2, "source_cameras": [0, 1],
          "keypoints_room": [[400.0, 100.0, 950.0], null, [398.0, 105.0, 948.0]],
          "keypoints_valid": [true, false, true],
          "reprojection_error_px": [3.0, null, 2.9],
          "reprojected_px": {
            "0": [[600.0, 320.0], null, [590.0, 318.0]],
            "1": [[610.0, 325.0], null, [600.0, 323.0]],
            "2": [[615.0, 330.0], null, [605.0, 328.0]]
          }
        }
      ]
    },
    {
      "tick": 1, "timestamp_ns": 1040000000,
      "people": [
        {
          "track_id": 0, "num_contributing_cameras": 2, "source_cameras": [1, 2],
          "keypoints_room": [[11.0, 21.0, 901.0], [13.0, 26.0, 896.0], [9.0, 26.0, 896.0]],
          "keypoints_valid": [true, true, true],
          "reprojection_error_px": [2.0, 1.9, 2.2],
          "reprojected_px": {
            "0": [[501.0, 301.0], [506.0, 296.0], [496.0, 296.0]],
            "1": [[481.0, 311.0], [486.0, 306.0], [476.0, 306.0]],
            "2": [[521.0, 291.0], [526.0, 286.0], [516.0, 286.0]]
          }
        }
      ]
    },
    {
      "tick": 2, "timestamp_ns": 1080000000,
      "people": []
    }
  ]
}
)JSON";

QString write_fixture(const QString& dirPath) {
    const QString path = dirPath + "/skeleton3d.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(kFixtureJson);
    return path;
}

// A single-person, single-tick fixture WITH "keypoints_room_smoothed" —
// distinct smoothed values from the raw ones, so a bug that silently reused
// keypoints_room for the smoothed field would be caught.
const char* kSmoothedFixtureJson = R"JSON(
{
  "schema": "mosaic-skeleton3d-v1",
  "source_videos": ["video/video_0.mp4"],
  "cameras": [{"index": 0, "position_room": [0.0, 0.0, 0.0]}],
  "keypoint_names": ["nose", "left_eye"],
  "skeleton_edges": [[0, 1]],
  "master_fps": 25.0,
  "params": {"min_cameras": 2, "smoothing_window": 5},
  "frames": [
    {
      "tick": 0, "timestamp_ns": 1000000000,
      "people": [
        {
          "track_id": 0, "num_contributing_cameras": 2, "source_cameras": [0],
          "keypoints_room": [[10.0, 20.0, 900.0], null],
          "keypoints_room_smoothed": [[11.5, 21.5, 899.0], null],
          "keypoints_valid": [true, false],
          "reprojection_error_px": [2.1, null],
          "reprojected_px": {"0": [[500.0, 300.0], null]}
        }
      ]
    }
  ]
}
)JSON";

QString write_smoothed_fixture(const QString& dirPath) {
    const QString path = dirPath + "/skeleton3d_smoothed.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(kSmoothedFixtureJson);
    return path;
}

} // namespace

TEST(Skeleton3DResult, LoadsValidFileWithFullSchema) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = Skeleton3DResult::load(write_fixture(dir.path()));

    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.source_videos(),
              QStringList({"video/video_0.mp4", "video/video_1.mp4", "video/video_2.mp4"}));
    ASSERT_EQ(result.cameras().size(), 3);
    EXPECT_EQ(result.cameras()[1].index, 1);
    EXPECT_DOUBLE_EQ(result.cameras()[1].positionRoom[0], 500.0);
    EXPECT_EQ(result.keypoint_names(), QStringList({"nose", "left_eye", "right_eye"}));
    ASSERT_EQ(result.skeleton_edges().size(), 2);
    EXPECT_EQ(result.skeleton_edges()[0].first, 0);
    EXPECT_EQ(result.skeleton_edges()[0].second, 1);
    EXPECT_DOUBLE_EQ(result.master_fps(), 25.0);

    ASSERT_EQ(result.frames().size(), 3);

    const auto& f0 = result.frames()[0];
    EXPECT_EQ(f0.tick, 0);
    EXPECT_EQ(f0.timestampNs, 1000000000);
    ASSERT_EQ(f0.people.size(), 2);

    const auto& p0 = f0.people[0];
    EXPECT_EQ(p0.trackId, 0);
    EXPECT_EQ(p0.numContributingCameras, 3);
    EXPECT_EQ(p0.sourceCameras, QVector<int>({0, 1, 2}));
    ASSERT_EQ(p0.keypoints.size(), 3);
    EXPECT_TRUE(p0.keypoints[0].valid);
    EXPECT_DOUBLE_EQ(p0.keypoints[0].positionRoom[0], 10.0);
    EXPECT_DOUBLE_EQ(p0.keypoints[0].reprojectionErrorPx, 2.1);
    ASSERT_EQ(p0.reprojectedPx.size(), 3);
    EXPECT_DOUBLE_EQ(p0.reprojectedPx.value(0)[0].x(), 500.0);

    // p1 (below) is only contributed to by cameras 0/1, but reprojected_px is
    // written for EVERY calibrated camera including non-contributing ones —
    // confirm camera 2's entry actually parses and isn't dropped/restricted
    // to source_cameras.
    const auto& p1 = f0.people[1];
    EXPECT_EQ(p1.sourceCameras, QVector<int>({0, 1}));
    ASSERT_TRUE(p1.reprojectedPx.contains(2));
    ASSERT_EQ(p1.reprojectedPx.value(2).size(), 3);
    EXPECT_DOUBLE_EQ(p1.reprojectedPx.value(2)[0].x(), 615.0);
}

TEST(Skeleton3DResult, MissingFileIsInvalidNotCrashing) {
    const auto result = Skeleton3DResult::load("Z:/does/not/exist/skeleton3d.json");
    EXPECT_FALSE(result.is_valid());
    EXPECT_TRUE(result.frames().isEmpty());
}

TEST(Skeleton3DResult, NearestFrameHandlesGapsAndBoundaries) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = Skeleton3DResult::load(write_fixture(dir.path()));
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
    EXPECT_EQ(result.nearest_frame(1020000000)->tick, 0);   // midpoint of tick 0/1
    EXPECT_EQ(result.nearest_frame(1060000000)->tick, 1);   // midpoint of tick 1/2
}

TEST(Skeleton3DResult, NearestFrameOnEmptyResultReturnsNull) {
    const Skeleton3DResult result;
    EXPECT_EQ(result.nearest_frame(0), nullptr);
}

TEST(Skeleton3DResult, EmptyPeopleFrameParsesToEmptyVectorNotDropped) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = Skeleton3DResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    ASSERT_EQ(result.frames().size(), 3);
    EXPECT_TRUE(result.frames()[2].people.isEmpty());
}

TEST(Skeleton3DResult, InvalidKeypointsHaveNullFieldsAcrossAllArrays) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = Skeleton3DResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    const auto& p1 = result.frames()[0].people[1];
    ASSERT_EQ(p1.keypoints.size(), 3);
    EXPECT_TRUE(p1.keypoints[0].valid);
    EXPECT_FALSE(p1.keypoints[1].valid);
    EXPECT_DOUBLE_EQ(p1.keypoints[1].reprojectionErrorPx, -1.0);
    // positionRoom at an invalid index must also stay at its default — not
    // just reprojectionErrorPx — since keypointsValid is documented as the
    // single source of truth across every parallel array.
    EXPECT_DOUBLE_EQ(p1.keypoints[1].positionRoom[0], 0.0);
    EXPECT_DOUBLE_EQ(p1.keypoints[1].positionRoom[1], 0.0);
    EXPECT_DOUBLE_EQ(p1.keypoints[1].positionRoom[2], 0.0);
    EXPECT_TRUE(p1.keypoints[2].valid);

    // A null entry in reprojected_px still parses to a default (null)
    // QPointF at that index, not a dropped/shorter vector — every camera's
    // vector stays index-aligned with keypoints() regardless of validity.
    // Checked across all three cameras' entries (0/1/2), not just camera 0,
    // since a bug could plausibly touch only one camera's array.
    ASSERT_EQ(p1.reprojectedPx.value(0).size(), 3);
    EXPECT_EQ(p1.reprojectedPx.value(0)[1], QPointF());
    ASSERT_EQ(p1.reprojectedPx.value(1).size(), 3);
    EXPECT_EQ(p1.reprojectedPx.value(1)[1], QPointF());
    ASSERT_EQ(p1.reprojectedPx.value(2).size(), 3);
    EXPECT_EQ(p1.reprojectedPx.value(2)[1], QPointF());
}

TEST(Skeleton3DResult, LoadsKeypointsRoomSmoothedWhenPresent) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = Skeleton3DResult::load(write_smoothed_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    const auto& kp0 = result.frames()[0].people[0].keypoints[0];
    EXPECT_TRUE(kp0.valid);
    EXPECT_DOUBLE_EQ(kp0.positionRoom[0], 10.0);
    // The smoothed value must be distinct from the raw one — confirms
    // "keypoints_room_smoothed" is really parsed from its own JSON field,
    // not silently aliased to keypoints_room.
    EXPECT_DOUBLE_EQ(kp0.positionRoomSmoothed[0], 11.5);
    EXPECT_DOUBLE_EQ(kp0.positionRoomSmoothed[1], 21.5);
    EXPECT_DOUBLE_EQ(kp0.positionRoomSmoothed[2], 899.0);

    // An invalid keypoint's smoothed value stays at the default too — same
    // single-source-of-truth discipline "keypoints_valid" already has for
    // every other parallel array.
    const auto& kp1 = result.frames()[0].people[0].keypoints[1];
    EXPECT_FALSE(kp1.valid);
    EXPECT_DOUBLE_EQ(kp1.positionRoomSmoothed[0], 0.0);
}

TEST(Skeleton3DResult, BackwardCompatFileWithNoSmoothedFieldFallsBackToRawPosition) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // kFixtureJson predates "keypoints_room_smoothed" entirely (no such key
    // anywhere) — confirms an older skeleton3d.json still loads cleanly,
    // and a caller that always renders positionRoomSmoothed sees the raw
    // value rather than a misleading {0,0,0} "at the room origin".
    const auto result = Skeleton3DResult::load(write_fixture(dir.path()));
    ASSERT_TRUE(result.is_valid());

    const auto& kp0 = result.frames()[0].people[0].keypoints[0];
    EXPECT_TRUE(kp0.valid);
    EXPECT_DOUBLE_EQ(kp0.positionRoomSmoothed[0], kp0.positionRoom[0]);
    EXPECT_DOUBLE_EQ(kp0.positionRoomSmoothed[1], kp0.positionRoom[1]);
    EXPECT_DOUBLE_EQ(kp0.positionRoomSmoothed[2], kp0.positionRoom[2]);
}
