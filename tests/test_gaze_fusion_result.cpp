#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "analysis/gaze_fusion_result.hpp"

using mosaic::GazeFusionResult;

namespace {

// An original single-subject (v1) file: tick 0 seen by 2 cameras with a
// plane target, tick 1 by 1 camera, tick 2 by none. Still loads, as subject
// S1, so results computed before the v2 rewrite stay viewable.
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

// What analysis/gaze/io_v2.py writes: two subjects looking at each other
// on tick 0 (mutual), S1 at a named region on tick 2 and without usable
// eyes on tick 4, analysed every 2nd tick at 25 fps.
const char* kV2Json = R"JSON(
{
  "schema": "mosaic-gaze-fusion-v2",
  "source": "synced", "fps": 25.0, "n_ticks": 6, "analysed_every": 2, "first_tick": 100,
  "source_videos": {"0": "synced/video_0.mp4", "1": "synced/video_1.mp4"},
  "annotated_videos": {"0": "gaze_fusion/video_0.gaze.mp4", "1": "gaze_fusion/video_1.gaze.mp4"},
  "room_video": "gaze_fusion/room_topdown.mp4",
  "cameras": [{"index": 0, "position_room": [0, 0, 0]}, {"index": 1, "position_room": [1800, 0, 1200]}],
  "plane": {"defined": false, "point": null, "normal": null},
  "regions": [{"name": "screen", "centre": [0, -200, 1800], "normal": [0, 0, -1],
               "u_axis": [1, 0, 0], "width": 600, "height": 340}],
  "subjects": [{"id": "S1", "name": "Child", "frames": 3, "face_scale": 1.04},
               {"id": "S2", "name": "S2", "frames": 2, "face_scale": null}],
  "frames": [
    {"tick": 0, "timestamp_ns": 4000000000, "video_frame_index": 0, "subjects": [
      {"id": "S1", "name": "Child", "origin": [-700, 0, 2500], "direction": [1, 0, 0],
       "point": [671, 0, 2500],
       "target": {"type": "subject", "label": "S2", "subject": "S2", "distance_mm": 1371, "measured": null},
       "mutual": true, "confidence": 0.8, "uncertainty_deg": 3.5, "n_cameras": 2,
       "per_camera": [{"camera": 0, "face_box_px": [500, 400, 580, 490], "direction": [1, 0, 0], "weight": 0.7},
                      {"camera": 1, "face_box_px": [900, 380, 990, 480], "direction": null, "weight": 0.0}]},
      {"id": "S2", "name": "S2", "origin": [700, 0, 2500], "direction": [-1, 0, 0],
       "point": [-671, 0, 2500],
       "target": {"type": "subject", "label": "Child", "subject": "S1", "distance_mm": 1371, "measured": null},
       "mutual": true, "confidence": 0.7, "uncertainty_deg": 4.0, "n_cameras": 1, "per_camera": []}
    ]},
    {"tick": 2, "timestamp_ns": 4080000000, "video_frame_index": 2, "subjects": [
      {"id": "S1", "name": "Child", "origin": [-700, 0, 2500], "direction": [0.3, 0, -0.95],
       "point": [0, -200, 1800],
       "target": {"type": "region", "label": "screen", "subject": null, "distance_mm": 990, "measured": null},
       "mutual": false, "confidence": 0.6, "uncertainty_deg": null, "n_cameras": 1, "per_camera": []}
    ]},
    {"tick": 4, "timestamp_ns": 4160000000, "video_frame_index": 4, "subjects": [
      {"id": "S1", "name": "Child", "origin": [-700, 0, 2500], "direction": null, "point": null,
       "target": null, "mutual": false, "confidence": 0.0, "uncertainty_deg": null, "n_cameras": 1,
       "per_camera": []}
    ]}
  ]
}
)JSON";

QString write_json(const QString& dirPath, const char* json) {
    const QString path = dirPath + "/gaze_fusion.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(json);
    return path;
}

} // namespace

// ── v1 ───────────────────────────────────────────────────────────────────────

TEST(GazeFusionResult, V1FilesLoadAsOneSubject) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = GazeFusionResult::load(write_json(dir.path(), kFixtureJson));
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.schema_version(), 1);
    EXPECT_EQ(result.source_videos(), QStringList({"video/video_0.mp4", "video/video_1.mp4"}));
    EXPECT_EQ(result.subject_ids(), QStringList{"S1"});
    ASSERT_EQ(result.cameras().size(), 2);
    EXPECT_DOUBLE_EQ(result.cameras()[1].positionRoom[0], 500.0);
    EXPECT_TRUE(result.plane_defined());
    EXPECT_DOUBLE_EQ(result.plane_point()[2], 1000.0);
    EXPECT_DOUBLE_EQ(result.master_fps(), 25.0);
    ASSERT_EQ(result.frames().size(), 3);

    const auto& f0 = result.frames()[0];
    EXPECT_EQ(f0.tick, 0);
    EXPECT_EQ(f0.timestampNs, 1000000000);
    ASSERT_EQ(f0.subjects.size(), 1);
    const auto& s0 = f0.subjects[0];
    EXPECT_EQ(s0.id, "S1");
    EXPECT_DOUBLE_EQ(s0.origin[0], 10.0);
    EXPECT_TRUE(s0.hasDirection);
    EXPECT_TRUE(s0.hasPoint);
    EXPECT_DOUBLE_EQ(s0.point[2], 1000.0);
    EXPECT_EQ(s0.targetType, "plane");
    EXPECT_EQ(s0.numCameras, 2);
    ASSERT_EQ(s0.perCamera.size(), 2);
    EXPECT_DOUBLE_EQ(s0.perCamera[0].faceBoxPx.left(), 10.0);
    EXPECT_DOUBLE_EQ(s0.perCamera[1].directionRoom[0], 0.01);

    EXPECT_FALSE(result.frames()[1].subjects[0].hasPoint); // one camera, no target
    EXPECT_TRUE(result.frames()[2].subjects.isEmpty());    // nobody seen
    EXPECT_TRUE(result.annotated_video(0).isEmpty());      // v1 rendered nothing
}

TEST(GazeFusionResult, MissingFileIsInvalidNotCrashing) {
    const auto result = GazeFusionResult::load("Z:/does/not/exist/gaze_fusion.json");
    EXPECT_FALSE(result.is_valid());
    EXPECT_TRUE(result.frames().isEmpty());
}

TEST(GazeFusionResult, NearestFrameHandlesGapsAndBoundaries) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = GazeFusionResult::load(write_json(dir.path(), kFixtureJson));
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.nearest_frame(1000000000)->tick, 0);
    EXPECT_EQ(result.nearest_frame(1040000000)->tick, 1);
    EXPECT_EQ(result.nearest_frame(0)->tick, 0);
    EXPECT_EQ(result.nearest_frame(9999999999)->tick, 2);
    EXPECT_EQ(result.nearest_frame(1020000000)->tick, 0); // ties go to the earlier frame
    // v1 has no video frame index: playback maps by time from the first tick.
    EXPECT_EQ(result.frame_at_position_ms(40)->tick, 1);
    EXPECT_EQ(result.frame_for_video_frame(1, 5), nullptr);
}

TEST(GazeFusionResult, NearestFrameOnEmptyResultReturnsNull) {
    const GazeFusionResult result;
    EXPECT_EQ(result.nearest_frame(0), nullptr);
    EXPECT_EQ(result.frame_at_position_ms(0), nullptr);
}

// ── v2 ───────────────────────────────────────────────────────────────────────

TEST(GazeFusionResult, V2LoadsSubjectsTargetsAndMutualGaze) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = GazeFusionResult::load(write_json(dir.path(), kV2Json));
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.schema_version(), 2);
    EXPECT_EQ(result.subject_ids(), (QStringList{"S1", "S2"}));
    EXPECT_EQ(result.subject_name("S1"), "Child");
    EXPECT_EQ(result.analysed_every(), 2);
    EXPECT_EQ(result.annotated_video(1), "gaze_fusion/video_1.gaze.mp4");
    EXPECT_TRUE(result.annotated_video(5).isEmpty());
    EXPECT_EQ(result.room_video(), "gaze_fusion/room_topdown.mp4");
    EXPECT_TRUE(result.source_videos().contains("synced/video_0.mp4"));
    ASSERT_EQ(result.regions().size(), 1);
    EXPECT_EQ(result.regions()[0].name, "screen");
    EXPECT_DOUBLE_EQ(result.regions()[0].width, 600.0);

    const auto& f0 = result.frames()[0];
    const auto* s1 = f0.subject("S1");
    const auto* s2 = f0.subject("S2");
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s1->name, "Child");
    EXPECT_EQ(s1->targetType, "subject");
    EXPECT_EQ(s1->targetSubject, "S2");
    EXPECT_TRUE(s1->mutual && s2->mutual);
    EXPECT_EQ(s2->targetLabel, "Child");
    EXPECT_DOUBLE_EQ(s1->uncertaintyDeg, 3.5);
    ASSERT_EQ(s1->perCamera.size(), 2);
    EXPECT_TRUE(s1->perCamera[0].hasDirection);
    EXPECT_FALSE(s1->perCamera[1].hasDirection);
    EXPECT_DOUBLE_EQ(s1->perCamera[1].faceBoxPx.right(), 990.0);

    const auto* region = result.frames()[1].subject("S1");
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->targetType, "region");
    EXPECT_EQ(region->targetLabel, "screen");
    EXPECT_DOUBLE_EQ(region->uncertaintyDeg, -1.0); // null

    const auto* noGaze = result.frames()[2].subject("S1");
    ASSERT_NE(noGaze, nullptr);
    EXPECT_FALSE(noGaze->hasDirection);
    EXPECT_FALSE(noGaze->hasPoint);
    EXPECT_TRUE(noGaze->targetType.isEmpty());
}

TEST(GazeFusionResult, V2PlaybackHoldsTheLastAnalysedFrame) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = GazeFusionResult::load(write_json(dir.path(), kV2Json));
    ASSERT_TRUE(result.is_valid());
    // Video frame 1 sits between analysed frames 0 and 2: hold frame 0.
    EXPECT_EQ(result.frame_for_video_frame(1, 2)->tick, 0);
    EXPECT_EQ(result.frame_for_video_frame(2, 2)->tick, 2);
    EXPECT_EQ(result.frame_for_video_frame(-1, 2), nullptr); // before the first
    EXPECT_EQ(result.frame_for_video_frame(40, 2), nullptr); // long after the last
    // Position 120 ms at 25 fps is video frame 3, so analysed frame 2.
    EXPECT_EQ(result.frame_at_position_ms(120)->tick, 2);
}
