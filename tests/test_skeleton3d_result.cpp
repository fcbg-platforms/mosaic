#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "analysis/room_view_math.hpp"
#include "analysis/skeleton3d_result.hpp"
#include "ui/analysis/room3d_scene.hpp"

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
    {"index": 1, "position_room": [500.0, 0.0, 100.0],
     "rotation_room": [[0, 0, 1], [0, 1, 0], [-1, 0, 0]], "fov_deg": [90.0, 58.7]},
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
    // Where camera 1 looks: the third column of its rotation, room +x.
    EXPECT_TRUE(result.cameras()[1].hasRotation);
    EXPECT_DOUBLE_EQ(result.cameras()[1].rotationRoom[2], 1.0);
    EXPECT_DOUBLE_EQ(result.cameras()[1].rotationRoom[6], -1.0);
    EXPECT_DOUBLE_EQ(result.cameras()[1].fovDeg[0], 90.0);
    // An older result without it.
    EXPECT_FALSE(result.cameras()[0].hasRotation);
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
    EXPECT_EQ(p0.trackId, 1); // no person_id in this file: track_id 0 + 1
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
    EXPECT_EQ(result.nearest_frame(1020000000)->tick, 0); // midpoint of tick 0/1
    EXPECT_EQ(result.nearest_frame(1060000000)->tick, 1); // midpoint of tick 1/2
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

// ── room_view: which way is up, and where the floor is ───────────────────

namespace {

// One person standing upright for `frames` ticks, in a room frame whose up
// is -y (a level reference camera): shoulders 1.4 m up, hips 0.9 m, ankles
// 8 cm.
Skeleton3DResult upright_person(QTemporaryDir& dir, int frames) {
    QString json = R"({"schema": "mosaic-skeleton3d-v1", "source_videos": [],
      "cameras": [{"index": 0, "position_room": [0, 0, 0]}],
      "keypoint_names": ["left_shoulder", "right_shoulder", "left_hip", "right_hip",
                         "left_ankle", "right_ankle"],
      "skeleton_edges": [], "master_fps": 25.0, "params": {}, "frames": [)";
    for (int t = 0; t < frames; ++t) {
        json += QString(R"(%1{"tick": %2, "timestamp_ns": %3, "people": [{"track_id": 0,
          "num_contributing_cameras": 2, "source_cameras": [0],
          "keypoints_room": [[-200, -1400, 3000], [200, -1400, 3000], [-150, -900, 3000],
                             [150, -900, 3000], [-100, -80, 3000], [100, -80, 3000]],
          "keypoints_valid": [true, true, true, true, true, true],
          "reprojection_error_px": [1, 1, 1, 1, 1, 1], "reprojected_px": {}}]})")
                    .arg(t ? "," : "")
                    .arg(t)
                    .arg(1000000000LL + 40000000LL * t);
    }
    json += "]}";
    const QString path = dir.path() + "/upright.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(json.toUtf8());
    f.close();
    return Skeleton3DResult::load(path);
}

} // namespace

TEST(RoomView, UpAndFloorComeFromThePeople) {
    QTemporaryDir dir;
    const auto r = upright_person(dir, 12);
    ASSERT_TRUE(r.is_valid());
    const auto up = mosaic::room_view::estimate_up(r);
    EXPECT_NEAR(up[1], -1.0, 1e-9);
    // Ankles 80 mm up, less the ankle's own height: the floor is at 0.
    EXPECT_NEAR(mosaic::room_view::floor_height(r, up), 0.0, 1e-6);
    // The Front view looks the way the reference camera does (+z).
    EXPECT_NEAR(mosaic::room_view::floor_forward(up)[2], 1.0, 1e-9);
}

TEST(RoomView, TooFewPeopleLeaveUpToTheCameras) {
    QTemporaryDir dir;
    // Two ticks are not enough to trust the people; the reference camera's
    // image-up (-y) decides.
    const auto r = upright_person(dir, 2);
    ASSERT_TRUE(r.is_valid());
    EXPECT_NEAR(mosaic::room_view::estimate_up(r)[1], -1.0, 1e-9);

    // A camera turned so its image-up is room -z, and nobody seen: up is
    // -z, the floor 2 m below the camera.
    QFile f(dir.path() + "/cam.json");
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(R"({"schema": "mosaic-skeleton3d-v1", "cameras": [{"index": 0,
      "position_room": [0, 0, -2500], "rotation_room": [[1, 0, 0], [0, 0, 1], [0, 1, 0]],
      "fov_deg": [60, 40]}], "keypoint_names": [], "skeleton_edges": [], "frames": [
      {"tick": 0, "timestamp_ns": 0, "people": []}]})");
    f.close();
    const auto cams = Skeleton3DResult::load(dir.path() + "/cam.json");
    ASSERT_TRUE(cams.is_valid());
    const auto up = mosaic::room_view::estimate_up(cams);
    EXPECT_NEAR(up[2], -1.0, 1e-9);
    EXPECT_NEAR(mosaic::room_view::floor_height(cams, up), 500.0, 1e-6);
}

TEST(Skeleton3DResult, PeopleJoinedFromTracksKeepTheirPersonNumber) {
    QTemporaryDir dir;
    QFile f(dir.filePath("p.json"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(R"({"schema": "mosaic-skeleton3d-v1", "cameras": [], "keypoint_names": ["nose"],
      "skeleton_edges": [], "frames": [
      {"tick": 0, "timestamp_ns": 0, "people": [{"track_id": 0, "person_id": 1,
        "keypoints_room": [[0,0,0]], "keypoints_valid": [true]}]},
      {"tick": 1, "timestamp_ns": 40000000, "people": [{"track_id": 4, "person_id": 1,
        "keypoints_room": [[0,0,0]], "keypoints_valid": [true]}]}]})");
    f.close();
    const auto r = Skeleton3DResult::load(dir.filePath("p.json"));
    ASSERT_TRUE(r.is_valid());
    // Track 4 continues person 1 (analysis/pose3d/people.py).
    EXPECT_EQ(r.frames()[1].people[0].trackId, 1);
}

// ── The 3D room view's scene (Room3DScene) ───────────────────────────────

TEST(Room3DScene, PeopleStandOnTheFloorAndCanBeHidden) {
    QTemporaryDir dir;
    const auto r = upright_person(dir, 12); // up is -y, ankles 80 mm up
    ASSERT_TRUE(r.is_valid());
    mosaic::Room3DScene scene;
    scene.set_result(r);
    scene.set_position_ms(0);

    // Y-up, the floor at 0 (80 mm below the ankles, an ankle's own height,
    // which puts it at room height 0 here): heights read straight off y.
    EXPECT_NEAR(scene.to_scene({100, -80, 3000}).y(), 80.0f, 1e-3f);
    EXPECT_NEAR(scene.to_scene({-200, -1400, 3000}).y(), 1400.0f, 1e-3f);
    // The reference camera's forward (room +z) is the scene's -z.
    EXPECT_NEAR(scene.dir_to_scene({0, 0, 1}).z(), -1.0f, 1e-6f);

    // One person: six joints (no face points, so no head), one name tag
    // above the shoulders, and the room coordinates on hover.
    ASSERT_EQ(scene.joints().size(), 6);
    ASSERT_EQ(scene.tags().size(), 1);
    EXPECT_EQ(scene.tags()[0].toMap()["text"].toString(), "Person 1");
    EXPECT_TRUE(scene.joints()[0].toMap()["info"].toString().contains("x -0.20"));
    EXPECT_EQ(scene.people().size(), 1);

    scene.set_shown(1, false);
    EXPECT_TRUE(scene.joints().isEmpty());
    EXPECT_TRUE(scene.tags().isEmpty());
    EXPECT_FALSE(scene.people()[0].toMap()["shown"].toBool());
}

TEST(Skeleton3DResult, PeopleSeenUnderASecondAreBrief) {
    // Person 1 in all 40 ticks (25 per second: 1.6 s); Person 2 in 3 (0.12 s),
    // a passing false detection.
    QString frames;
    for (int t = 0; t < 40; ++t) {
        QString people = R"({"track_id": 0, "person_id": 1, "keypoints_room": [[0,0,3000]], )"
                         R"("keypoints_valid": [true]})";
        if (t >= 10 && t < 13) {
            people += R"(, {"track_id": 1, "person_id": 2, "keypoints_room": [[900,0,3000]], )"
                      R"("keypoints_valid": [true]})";
        }
        frames += QString(R"(%1{"tick": %2, "timestamp_ns": %3, "people": [%4]})")
                      .arg(t ? "," : "")
                      .arg(t)
                      .arg(40000000LL * t)
                      .arg(people);
    }
    QTemporaryDir dir;
    QFile f(dir.filePath("b.json"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(
        QString(R"({"schema": "mosaic-skeleton3d-v1", "cameras": [], "keypoint_names": ["nose"], )"
                R"("skeleton_edges": [], "frames": [%1]})")
            .arg(frames)
            .toUtf8());
    f.close();
    const auto r = Skeleton3DResult::load(dir.filePath("b.json"));
    ASSERT_TRUE(r.is_valid());
    const auto seconds = r.seconds_by_person();
    EXPECT_NEAR(seconds.value(1), 1.6, 0.05);
    EXPECT_NEAR(seconds.value(2), 0.12, 0.01);
    EXPECT_EQ(r.lasting_people(), QList<int>{1});

    // The room view lists Person 2 as brief, hidden until shown.
    mosaic::Room3DScene scene;
    scene.set_result(r);
    const auto people = scene.people();
    ASSERT_EQ(people.size(), 2);
    EXPECT_TRUE(people[0].toMap()["shown"].toBool());
    EXPECT_FALSE(people[1].toMap()["shown"].toBool());
    EXPECT_TRUE(people[1].toMap()["brief"].toBool());
}

TEST(Room3DScene, PathsCoverTheWholeRecordingButNoGap) {
    // Person 1's hips walk along room x, 100 mm a tick at 25 per second,
    // for 10 ticks; 1 s later, 10 more; then 10 ticks drifting 3 mm a tick:
    // a point only once 20 mm from the last one drawn (after 7 ticks), not
    // from the tick before. 30 ticks: 1.2 s in 3D, so Person 1 is shown. One
    // camera, its fields of view 60 by 40 degrees.
    QStringList frames;
    int tick       = 0;
    const auto add = [&](int64_t ns, double x) {
        frames << QString(R"({"tick": %1, "timestamp_ns": %2, "people": [{"track_id": 0, )"
                          R"("person_id": 1, "keypoints_room": [[%3,-900,3000],[%4,-900,3000]], )"
                          R"("keypoints_valid": [true, true]}]})")
                      .arg(tick++)
                      .arg(ns)
                      .arg(x - 150)
                      .arg(x + 150);
    };
    for (int k = 0; k < 10; ++k) {
        add(40000000LL * k, 100.0 * k);
    }
    for (int k = 0; k < 10; ++k) {
        add(1400000000LL + 40000000LL * k, 2000.0 + 100.0 * k);
    }
    for (int k = 1; k <= 10; ++k) {
        add(1760000000LL + 40000000LL * k, 2900.0 + 3.0 * k);
    }
    QTemporaryDir dir;
    QFile f(dir.filePath("w.json"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QString(R"({"schema": "mosaic-skeleton3d-v1", "cameras": [{"index": 0, )"
                    R"("position_room": [0, 0, 0], "rotation_room": [[1,0,0],[0,1,0],[0,0,1]], )"
                    R"("fov_deg": [60, 40]}], "keypoint_names": ["left_hip", "right_hip"], )"
                    R"("skeleton_edges": [], "frames": [%1]})")
                .arg(frames.join(","))
                .toUtf8());
    f.close();
    const auto r = Skeleton3DResult::load(dir.filePath("w.json"));
    ASSERT_TRUE(r.is_valid());

    mosaic::Room3DScene scene;
    scene.set_result(r);
    ASSERT_TRUE(scene.people()[0].toMap()["shown"].toBool());
    EXPECT_TRUE(scene.mesh("paths").vertices.empty()); // off until asked for
    scene.set_paths(true);
    // 9 steps in each walk, none across the 1 s gap, one in the drift: 19
    // lines of 2 points, 7 numbers each (x y z r g b a), on the floor.
    const auto& v = scene.mesh("paths").vertices;
    ASSERT_EQ(v.size(), 19u * 2 * 7);
    EXPECT_NEAR(v[1], 3.0f, 1e-6f);
    EXPECT_NEAR(v[7] - v[0], 100.0f, 1e-3f); // the first step, along the room's right
    // A hidden person has no path.
    scene.set_shown(1, false);
    EXPECT_TRUE(scene.mesh("paths").vertices.empty());

    // The view from the camera needs its vertical field of view.
    ASSERT_EQ(scene.cameras().size(), 1);
    EXPECT_NEAR(scene.cameras()[0].toMap()["fovY"].toDouble(), 40.0, 1e-9);
}
