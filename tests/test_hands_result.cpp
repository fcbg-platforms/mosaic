#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <cmath>

#include "analysis/hands_result.hpp"

using mosaic::HandsResult;

namespace {

// The schema analysis/run_hands.py writes (analyse_camera()), cut down.
const char* kFixtureJson = R"JSON(
{
 "schema": "mosaic-hands-v1",
 "camera_index": 4,
 "model": "rtmw",
 "stride": 2,
 "fps": 21.0,
 "video_start_ms": 5000.0,
 "keypoint_names": ["wrist"],
 "hands": [
  {"subject_id": 3, "side": "left", "frames": 30, "seen_ms": 1200.0,
   "seen_spans": [[5000.0, 5800.0], [7000.0, 7400.0]], "median_size_px": 170.5,
   "mean_score": 0.41,
   "kinematics": {"measured_s": 12.1, "wrist_path_hl": 31.2, "wrist_speed_median_hl_s": 1.62,
                  "wrist_speed_p95_hl_s": 7.43, "grip_aperture_median_palm": 0.46,
                  "grip_aperture_p95_palm": 0.9, "hand_opening_median_palm": null},
   "movements": {"movements": 10, "duration_median_s": 0.88, "peak_speed_median_hl_s": 6.66,
                 "time_to_peak_median_pct": 47.0, "movement_units_median": 3.0,
                 "sparc_median": -1.95, "straightness_median": 0.77,
                 "max_grip_aperture_median_palm": null}},
  {"subject_id": null, "side": "unknown", "frames": 2, "seen_ms": 80.0,
   "seen_spans": [[9000.0, 9080.0]], "median_size_px": 90.0, "mean_score": 0.6}
 ],
 "annotated_video": "hands/video_4.hands.mp4",
 "kinematics_file": "hands/video_4.hands.kinematics.csv",
 "movements_file": "hands/video_4.hands.movements.csv",
 "people": [{"subject_id": 3, "laterality_index": 0.05, "both_moving_s": 6.03,
             "both_moving_share": 0.51}],
 "frames_file": "hands/video_4.hands.frames.json"
}
)JSON";

HandsResult load_fixture(QTemporaryDir& dir, const char* json) {
    const QString path = dir.filePath("video_4.hands.json");
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(json);
    f.close();
    return HandsResult::load(path);
}

} // namespace

TEST(HandsResult, ReadsTheModelAndEachHand) {
    QTemporaryDir dir;
    const HandsResult r = load_fixture(dir, kFixtureJson);
    ASSERT_TRUE(r.is_valid());
    EXPECT_EQ(r.model(), "rtmw");
    EXPECT_EQ(r.stride(), 2);
    EXPECT_DOUBLE_EQ(r.fps(), 21.0);
    EXPECT_DOUBLE_EQ(r.video_start_ms(), 5000.0);
    EXPECT_EQ(r.annotated_video(), "hands/video_4.hands.mp4");
    ASSERT_EQ(r.hands().size(), 2);
    const auto& left = r.hands()[0];
    EXPECT_EQ(left.subjectId, 3);
    EXPECT_EQ(left.side, "left");
    EXPECT_EQ(left.display_name(), "S3 left hand");
    EXPECT_DOUBLE_EQ(left.seenMs, 1200.0);
    EXPECT_DOUBLE_EQ(left.medianSizePx, 170.5);
    EXPECT_DOUBLE_EQ(left.meanScore, 0.41);
    ASSERT_EQ(left.seenSpans.size(), 2);
    EXPECT_DOUBLE_EQ(left.seenSpans[1].first, 7000.0);
    EXPECT_DOUBLE_EQ(left.seenSpans[1].second, 7400.0);
    EXPECT_EQ(r.kinematics_file(), "hands/video_4.hands.kinematics.csv");
    const auto& k = left.kinematics;
    ASSERT_TRUE(k.valid);
    EXPECT_DOUBLE_EQ(k.measuredS, 12.1);
    EXPECT_DOUBLE_EQ(k.wristSpeedP95, 7.43);
    EXPECT_DOUBLE_EQ(k.gripApertureMedian, 0.46);
    EXPECT_TRUE(std::isnan(k.handOpeningMedian)); // null: not measured
    const auto& m = left.movements;
    ASSERT_TRUE(m.valid);
    EXPECT_EQ(m.count, 10);
    EXPECT_DOUBLE_EQ(m.peakSpeed, 6.66);
    EXPECT_DOUBLE_EQ(m.movementUnits, 3.0);
    EXPECT_DOUBLE_EQ(m.sparc, -1.95);
    EXPECT_TRUE(std::isnan(m.maxGripAperture));
    EXPECT_EQ(r.movements_file(), "hands/video_4.hands.movements.csv");
    ASSERT_EQ(r.people().size(), 1);
    EXPECT_EQ(r.people()[0].subjectId, 3);
    EXPECT_DOUBLE_EQ(r.people()[0].lateralityIndex, 0.05);
    EXPECT_DOUBLE_EQ(r.people()[0].bothMovingShare, 0.51);
    // A hand no wrist was near: no person, no side, no kinematics.
    EXPECT_FALSE(r.hands()[1].kinematics.valid);
    EXPECT_FALSE(r.hands()[1].movements.valid);
    EXPECT_EQ(r.hands()[1].subjectId, -1);
    EXPECT_EQ(r.hands()[1].display_name(), "? hand");
}

TEST(HandsResult, ReadsTheKinematicsPerHand) {
    QTemporaryDir dir;
    const QString path = dir.filePath("video_4.hands.kinematics.csv");
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(
        "time_ms,subject_id,side,wrist_speed_hl_s,grip_aperture_palm\n"
        "100.0,3,left,1.5,0.4\n"
        "140.0,3,left,,0.5\n"
        "100.0,5,right,2.0,\n"
        "bad line\n");
    f.close();
    const auto series = HandsResult::load_kinematics(path);
    ASSERT_EQ(series.size(), 2);
    EXPECT_EQ(series[0].subjectId, 3);
    EXPECT_EQ(series[0].side, "left");
    EXPECT_EQ(series[0].timeMs, (QVector<double>{100.0, 140.0}));
    const auto& speed = series[0].columns["wrist_speed_hl_s"];
    ASSERT_EQ(speed.size(), 2);
    EXPECT_DOUBLE_EQ(speed[0], 1.5);
    EXPECT_TRUE(std::isnan(speed[1])); // empty: not measured
    EXPECT_EQ(series[1].side, "right");
    EXPECT_TRUE(std::isnan(series[1].columns["grip_aperture_palm"][0]));
    EXPECT_TRUE(HandsResult::load_kinematics(dir.filePath("missing.csv")).isEmpty());
    // The header only: a camera where no hand could be measured.
    QFile empty(dir.filePath("empty.csv"));
    ASSERT_TRUE(empty.open(QIODevice::WriteOnly | QIODevice::Text));
    empty.write("time_ms,subject_id,side,wrist_speed_hl_s\n");
    empty.close();
    EXPECT_TRUE(HandsResult::load_kinematics(dir.filePath("empty.csv")).isEmpty());
}

TEST(HandsResult, ReadsTheMovements) {
    QTemporaryDir dir;
    const QString path = dir.filePath("video_4.hands.movements.csv");
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(
        "subject_id,side,start_ms,end_ms,duration_s,peak_speed_hl_s,sparc\n"
        "3,left,1000.0,1880.0,0.88,9.4,-1.41\n"
        "3,right,2500.0,3100.0,0.6,4.2,-2.0\n");
    f.close();
    const auto moves = HandsResult::load_movements(path);
    ASSERT_EQ(moves.size(), 2);
    EXPECT_EQ(moves[0].subjectId, 3);
    EXPECT_EQ(moves[0].side, "left");
    EXPECT_DOUBLE_EQ(moves[0].startMs, 1000.0);
    EXPECT_DOUBLE_EQ(moves[0].endMs, 1880.0);
    EXPECT_DOUBLE_EQ(moves[1].peakSpeed, 4.2);
    EXPECT_TRUE(HandsResult::load_movements(dir.filePath("missing.csv")).isEmpty());
}

TEST(HandsResult, AnotherSchemaOrNoFileIsInvalid) {
    QTemporaryDir dir;
    EXPECT_FALSE(load_fixture(dir, R"({"schema": "mosaic-objects-v1"})").is_valid());
    EXPECT_FALSE(HandsResult::load(dir.filePath("missing.json")).is_valid());
}
