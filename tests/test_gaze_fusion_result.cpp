#include <gtest/gtest.h>

#include <QColor>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVector3D>

#include "analysis/gaze_fusion_result.hpp"
#include "analysis/gaze_pose_match.hpp"
#include "analysis/gaze_summary.hpp"
#include "gaze3d_fixture.hpp"
#include "ui/analysis/gaze3d_scene.hpp"

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

// ── The 3D Gaze charts' summary (summarize_gaze) ───────────────────────────

TEST(GazeSummary, TimeOnTargetsStretchesAndMutualGaze) {
    // Two subjects, 20 analysed ticks 80 ms apart. Ticks 0-4: mutual gaze.
    // 5-9: S1 at a region named "S2" (as S2 is: still another target), S2's
    // gaze on nothing. 10-11: S1 not seen, S2 without eyes. 12-14: S1 at S2,
    // not mutual (S2 at the screen). 15-19: mutual again.
    const auto sample = [](const char* id, const char* name, const QString& type,
                           const QString& label, const QString& subject, bool mutual,
                           bool eyes = true) {
        const QString target =
            type.isEmpty()
                ? QString("null")
                : QString(R"({"type": "%1", "label": "%2", "subject": %3})")
                      .arg(type, label,
                           subject.isEmpty() ? QString("null") : QString("\"%1\"").arg(subject));
        return QString(R"({"id": "%1", "name": "%2", "origin": [0, 0, 2500], )"
                       R"("direction": %3, "point": null, "target": %4, "mutual": %5, )"
                       R"("confidence": 0.8, "n_cameras": 2, "per_camera": []})")
            .arg(id, name, eyes ? "[1, 0, 0]" : "null", target, mutual ? "true" : "false");
    };
    QStringList frames;
    for (int k = 0; k < 20; ++k) {
        QStringList subjects;
        const bool mutual = k < 5 || k >= 15;
        if (k < 10 || k >= 12) {
            subjects << (k >= 5 && k < 10 ? sample("S1", "Child", "region", "S2", "", false)
                                          : sample("S1", "Child", "subject", "S2", "S2", mutual));
        }
        if (mutual) {
            subjects << sample("S2", "S2", "subject", "Child", "S1", true);
        } else if (k < 10) {
            subjects << sample("S2", "S2", "none", "none", "", false);
        } else if (k < 12) {
            subjects << sample("S2", "S2", "", "", "", false, false);
        } else {
            subjects << sample("S2", "S2", "region", "screen", "", false);
        }
        frames << QString(R"({"tick": %1, "timestamp_ns": %2, "video_frame_index": %1, )"
                          R"("subjects": [%3]})")
                      .arg(2 * k)
                      .arg(80000000LL * k)
                      .arg(subjects.join(","));
    }
    QTemporaryDir dir;
    QFile f(dir.filePath("g.json"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QString(R"({"schema": "mosaic-gaze-fusion-v2", "fps": 25.0, "analysed_every": 2, )"
                    R"("cameras": [], "plane": {"defined": false}, "regions": [], "subjects": [)"
                    R"({"id": "S1", "name": "Child"}, {"id": "S2", "name": "S2"}], )"
                    R"("frames": [%1]})")
                .arg(frames.join(","))
                .toUtf8());
    f.close();
    const auto r = GazeFusionResult::load(dir.filePath("g.json"));
    ASSERT_TRUE(r.is_valid());

    const auto g = mosaic::summarize_gaze(r);
    EXPECT_NEAR(g.stepS, 0.08, 1e-12);
    ASSERT_EQ(g.subjects.size(), 2);
    const auto seconds = [](const mosaic::GazeSubjectSummary& s, const QString& key) {
        for (const auto& t : s.secondsByTarget) {
            if (t.target.key == key) {
                return t.seconds;
            }
        }
        return 0.0;
    };

    // S1: 18 ticks seen; the person S2 13 of them, the region "S2" 5, kept
    // apart; longest first, shown by name.
    const auto& s1 = g.subjects[0];
    EXPECT_EQ(s1.name, "Child");
    EXPECT_NEAR(s1.secondsSeen, 1.44, 1e-9);
    EXPECT_NEAR(seconds(s1, "subject/S2"), 1.04, 1e-9);
    EXPECT_NEAR(seconds(s1, "region/S2"), 0.40, 1e-9);
    EXPECT_EQ(s1.secondsByTarget[0].target.key, "subject/S2");
    EXPECT_EQ(s1.secondsByTarget[0].target.label, "S2");
    // Stretches: S2, the region, then S2 again from tick 12 to 19 (the gap
    // before it ends the region's stretch; 12-19 is one).
    ASSERT_EQ(s1.spans.size(), 3);
    EXPECT_EQ(s1.spans[0].target.key, "subject/S2");
    EXPECT_EQ(s1.spans[0].startNs, 0);
    EXPECT_EQ(s1.spans[0].endNs, 400000000);
    EXPECT_EQ(s1.spans[1].target.key, "region/S2");
    EXPECT_EQ(s1.spans[2].startNs, 960000000);
    EXPECT_EQ(s1.spans[2].endNs, 1600000000);

    // S2: all 20 ticks: Child (S1) 10, nothing 5, the screen 3, no gaze 2.
    const auto& s2 = g.subjects[1];
    EXPECT_NEAR(s2.secondsSeen, 1.60, 1e-9);
    EXPECT_NEAR(seconds(s2, "subject/S1"), 0.80, 1e-9);
    EXPECT_NEAR(seconds(s2, mosaic::kGazeNothing), 0.40, 1e-9);
    EXPECT_NEAR(seconds(s2, "region/screen"), 0.24, 1e-9);
    EXPECT_NEAR(seconds(s2, mosaic::kGazeNoGaze), 0.16, 1e-9);

    // Mutual gaze: two episodes of 5 ticks; looking without being looked
    // back at (ticks 12-14) does not count.
    ASSERT_EQ(g.pairs.size(), 1);
    EXPECT_EQ(g.pairs[0].nameA, "Child");
    EXPECT_NEAR(g.pairs[0].seconds, 0.80, 1e-9);
    EXPECT_EQ(g.pairs[0].spans.size(), 2);
    EXPECT_NEAR(g.pairs[0].longestS, 0.40, 1e-9);
}

// ── Which 3D Pose person a 3D Gaze subject is (match_gaze_to_pose) ──────

TEST(GazePoseMatch, SubjectsMatchThePersonWhoseHeadIsThere) {
    // 3D Pose: Person 1 at x = -700, Person 2 at x = +700 (their face
    // points), 20 ticks 40 ms apart. 3D Gaze, on every other tick: S1 at
    // Person 2's head (8 cm off), S2 at Person 1's, and S3 at 0.5 m from
    // everyone (a false face, or someone 3D Pose did not see).
    QStringList poseFrames, gazeFrames;
    for (int k = 0; k < 20; ++k) {
        const auto person = [](int track, double x) {
            return QString(
                       R"({"track_id": %1, "keypoints_room": [[%2,-1600,2500],)"
                       R"([%3,-1600,2500],[%4,-1600,2500]], "keypoints_valid": [true,true,true]})")
                .arg(track)
                .arg(x)
                .arg(x - 75)
                .arg(x + 75);
        };
        // Person 3, for the last 12 ticks only (6 gaze ticks), 2 cm from
        // S1: nearer than Person 2, but there for a moment (a split track).
        poseFrames << QString(R"({"tick": %1, "timestamp_ns": %2, "people": [%3, %4%5]})")
                          .arg(k)
                          .arg(40000000LL * k)
                          .arg(person(0, -700), person(1, 700),
                               k >= 8 ? "," + person(2, 760) : QString());
        if (k % 2 == 0) {
            const auto subject = [](const char* id, double x) {
                return QString(R"({"id": "%1", "name": "%1", "origin": [%2, -1600, 2500], )"
                               R"("direction": [0, 0, 1], "n_cameras": 2})")
                    .arg(id)
                    .arg(x);
            };
            gazeFrames << QString(R"({"tick": %1, "timestamp_ns": %2, "subjects": [%3, %4, %5]})")
                              .arg(k)
                              .arg(40000000LL * k)
                              .arg(subject("S1", 780), subject("S2", -700), subject("S3", 0));
        }
    }
    QTemporaryDir dir;
    QFile pf(dir.filePath("s.json"));
    ASSERT_TRUE(pf.open(QIODevice::WriteOnly));
    pf.write(QString(R"({"schema": "mosaic-skeleton3d-v1", "cameras": [], "keypoint_names": )"
                     R"(["nose", "left_eye", "right_eye"], "skeleton_edges": [], "frames": [%1]})")
                 .arg(poseFrames.join(","))
                 .toUtf8());
    pf.close();
    QFile gf(dir.filePath("g.json"));
    ASSERT_TRUE(gf.open(QIODevice::WriteOnly));
    gf.write(QString(R"({"schema": "mosaic-gaze-fusion-v2", "fps": 25.0, "analysed_every": 2, )"
                     R"("cameras": [], "plane": {"defined": false}, "regions": [], "subjects": [)"
                     R"({"id": "S1"}, {"id": "S2"}, {"id": "S3"}], "frames": [%1]})")
                 .arg(gazeFrames.join(","))
                 .toUtf8());
    gf.close();
    const auto pose = mosaic::Skeleton3DResult::load(dir.filePath("s.json"));
    const auto gaze = GazeFusionResult::load(dir.filePath("g.json"));
    ASSERT_TRUE(pose.is_valid());
    ASSERT_TRUE(gaze.is_valid());

    const auto m = mosaic::match_gaze_to_pose(gaze, pose);
    ASSERT_EQ(m.size(), 3);
    EXPECT_EQ(m[0].subject, "S1");
    EXPECT_EQ(m[0].person, 2);
    EXPECT_NEAR(m[0].medianMm, 80.0, 1e-6);
    EXPECT_EQ(m[0].sharedTicks, 10); // Person 2 all along, not Person 3's moment
    EXPECT_EQ(m[1].person, 1);
    // S3 is 0.5 m from both: no match, rather than a guess.
    EXPECT_EQ(m[2].person, -1);
    // A limit tighter than anyone's distance (Person 3's 2 cm) leaves S1
    // unmatched too.
    EXPECT_EQ(mosaic::match_gaze_to_pose(gaze, pose, 10.0)[0].person, -1);

    // The everyone table: a row per tick and person in the frame, as many
    // columns as the header; Person 2's gaze is S1's (paired with the gaze
    // tick within 50 ms, so every pose tick here), Person 3's empty.
    QString csv;
    QTextStream ts(&csv);
    mosaic::write_people_csv(ts, pose, gaze);
    ts.flush();
    const QStringList lines = csv.split('\n', Qt::SkipEmptyParts);
    ASSERT_GE(lines.size(), 3);
    EXPECT_TRUE(lines[0].startsWith("# ") && lines[0].contains("S1 = Person 2"));
    const int columns = static_cast<int>(lines[1].split(',').size());
    EXPECT_TRUE(lines[1].endsWith(",gaze_subject,gaze_target_type,gaze_target,gaze_mutual"));
    int person2 = 0, person3 = 0;
    for (int i = 2; i < lines.size(); ++i) {
        const QStringList f = lines[i].split(',');
        ASSERT_EQ(f.size(), columns) << lines[i].toStdString();
        if (f[2] == "2") {
            ++person2;
            EXPECT_EQ(f[columns - 4], "S1");
            EXPECT_EQ(f[columns - 1], "false");
        } else if (f[2] == "3") {
            ++person3;
            EXPECT_TRUE(f[columns - 4].isEmpty() && f[columns - 1].isEmpty());
        }
    }
    EXPECT_EQ(person2, 20); // every tick: in the frame
    EXPECT_EQ(person3, 12); // only where in the frame
}

// ── The 3D Gaze view's scene (Gaze3DScene) ───────────────────────────────

TEST(Gaze3DScene, HeadsRaysAndTargetsInTheRoom) {
    QTemporaryDir dir;
    ASSERT_TRUE(write_gaze3d_fixture(dir.filePath("g.json")));
    const auto r = GazeFusionResult::load(dir.filePath("g.json"));
    ASSERT_TRUE(r.is_valid());
    // The cameras say which way they look, as skeleton3d.json's do.
    ASSERT_EQ(r.cameras().size(), 2);
    EXPECT_TRUE(r.cameras()[1].hasRotation);
    EXPECT_DOUBLE_EQ(r.cameras()[1].fovDeg[1], 40.0);

    mosaic::Gaze3DScene scene;
    scene.set_result(r);
    ASSERT_EQ(scene.stage().cameras().size(), 2);
    // Up is the table's normal turned towards the cameras (room -y); the
    // floor 2 m below the cameras: the heads, at the cameras' height, are
    // 2 m up.
    scene.set_position_ms(0);
    ASSERT_EQ(scene.heads().size(), 2);
    EXPECT_NEAR(scene.heads()[0].toMap()["pos"].value<QVector3D>().y(), 2000.0f, 1e-3f);

    // Frame 0: mutual gaze, a ray each, from the eyes to the other's face.
    ASSERT_EQ(scene.rays().size(), 2);
    const auto ray = scene.rays()[0].toMap();
    EXPECT_TRUE(ray["mutual"].toBool());
    EXPECT_NEAR(ray["len"].toDouble(), 1400.0, 1e-3);
    EXPECT_EQ(scene.tags()[0].toMap()["text"].toString(),
              QString("Child %1 S2").arg(QChar(0x2194))); // as on the video
    EXPECT_EQ(scene.tick(), 0);
    EXPECT_FALSE(scene.regions()[0].toMap()["lit"].toBool());

    // Frame 5 (0.4 s, tick 10): Child at the screen, S2 at the table: both
    // lit, each in the looker's colour (the video's: S1 orange-yellow).
    scene.set_position_ms(400);
    EXPECT_EQ(scene.tick(), 10);
    const auto screen = scene.regions()[0].toMap();
    EXPECT_TRUE(screen["lit"].toBool());
    EXPECT_EQ(screen["color"].value<QColor>(), scene.heads()[0].toMap()["color"].value<QColor>());
    EXPECT_EQ(scene.heads()[0].toMap()["color"].value<QColor>(), QColor(255, 200, 60));
    EXPECT_TRUE(scene.plane()["lit"].toBool());
    EXPECT_EQ(scene.points().size(), 2);
    // The gaze points of the ticks before trail behind.
    EXPECT_FALSE(scene.mesh("trails").vertices.empty());

    // Frame 8: Child not seen, S2 without eyes: a head, no ray.
    scene.set_position_ms(640);
    EXPECT_EQ(scene.heads().size(), 1);
    EXPECT_TRUE(scene.rays().isEmpty());
    EXPECT_TRUE(scene.heads()[0].toMap()["info"].toString().contains("no gaze"));

    // Hidden: left out.
    scene.set_position_ms(0);
    scene.set_shown("S1", false);
    EXPECT_EQ(scene.heads().size(), 1);
    EXPECT_FALSE(scene.people()[0].toMap()["shown"].toBool());

    // The 3D Pose room, when given, sets the floor: here 1.5 m below the
    // cameras.
    mosaic::Room3DStage::Basis room;
    room.floorH = -1500.0f;
    scene.set_result(r, room);
    scene.set_position_ms(0);
    EXPECT_NEAR(scene.heads()[0].toMap()["pos"].value<QVector3D>().y(), 1500.0f, 1e-3f);

    // A plane on a wall (its normal level, room +z) is not taken for up:
    // the cameras' up stands, and the screen, at the cameras' height but
    // further away, stays 2 m up (with +z as up it would not).
    QFile wall(dir.filePath("wall.json"));
    ASSERT_TRUE(wall.open(QIODevice::WriteOnly));
    wall.write(
        gaze3d_fixture_json().replace(R"("normal": [0, 1, 0])", R"("normal": [0, 0, 1])").toUtf8());
    wall.close();
    scene.set_result(GazeFusionResult::load(dir.filePath("wall.json")));
    scene.set_position_ms(0);
    EXPECT_NEAR(scene.regions()[0].toMap()["pos"].value<QVector3D>().y(), 2000.0f, 1e-3f);

    // Older results: cameras without directions.
    ASSERT_TRUE(write_gaze3d_fixture(dir.filePath("old.json"), false));
    const auto old = GazeFusionResult::load(dir.filePath("old.json"));
    ASSERT_TRUE(old.is_valid());
    EXPECT_FALSE(old.cameras()[0].hasRotation);
}
