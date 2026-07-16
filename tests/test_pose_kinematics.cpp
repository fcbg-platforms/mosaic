#include "analysis/pose_kinematics.hpp"
#include <gtest/gtest.h>
#include <QFile>
#include <QTemporaryDir>
#include <cmath>

using mosaic::compute_kinematics;
using mosaic::KinematicsSeries;
using mosaic::PoseAnalysisResult;

namespace {

// One frame's JSON object. hasSubject=false omits the subjects array
// entirely (empty list), matching a frame where detection failed outright.
// Matches the exact schema run_pose.py writes (mirrored by
// test_pose_analysis_result.cpp's fixture).
QString frame_json(int frameIndex, int64_t timestampNs, bool hasSubject,
                    double x, double y, double visibility) {
    if (!hasSubject) {
        return QString(R"({"frame_index": %1, "timestamp_ns": %2, "camera_index": 0,
            "inference_ms": 1.0, "backend": "test", "subjects": []})")
            .arg(frameIndex).arg(timestampNs);
    }
    return QString(R"({"frame_index": %1, "timestamp_ns": %2, "camera_index": 0,
        "inference_ms": 1.0, "backend": "test",
        "subjects": [{"subject_id": 0, "confidence": 0.9,
            "bbox_xyxy": [0.0, 0.0, 1.0, 1.0],
            "keypoints": [[%3, %4]], "visibilities": [%5]}]})")
        .arg(frameIndex).arg(timestampNs).arg(x).arg(y).arg(visibility);
}

PoseAnalysisResult load_frames(const QTemporaryDir& dir, const QStringList& frames) {
    const QString path = dir.path() + "/video_0.pose.json";
    const QString json = QString(R"({"source_video": "video_0.mp4",
        "keypoint_names": ["nose"], "skeleton_edges": [], "frames": [%1]})")
        .arg(frames.join(","));

    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(json.toUtf8());
    f.close();

    return PoseAnalysisResult::load(path);
}

constexpr int64_t kOneSecondNs = 1'000'000'000;

} // namespace

TEST(PoseKinematics, ConstantVelocityGivesConstantSpeed) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = load_frames(dir, {
        frame_json(0, 0 * kOneSecondNs, true, 0.0, 0.0, 1.0),
        frame_json(1, 1 * kOneSecondNs, true, 10.0, 0.0, 1.0),
        frame_json(2, 2 * kOneSecondNs, true, 20.0, 0.0, 1.0),
        frame_json(3, 3 * kOneSecondNs, true, 30.0, 0.0, 1.0),
    });
    ASSERT_TRUE(result.is_valid());

    const KinematicsSeries series = compute_kinematics(result, 0, 0, /*smoothingWindow=*/1);

    ASSERT_EQ(series.samples.size(), 4);
    EXPECT_TRUE(std::isnan(series.samples[0].speedPxPerS));
    EXPECT_DOUBLE_EQ(series.samples[1].speedPxPerS, 10.0);
    EXPECT_DOUBLE_EQ(series.samples[2].speedPxPerS, 10.0);
    EXPECT_DOUBLE_EQ(series.samples[3].speedPxPerS, 10.0);

    // Constant speed → zero acceleration at every defined sample.
    EXPECT_TRUE(std::isnan(series.samples[1].accelPxPerS2));
    EXPECT_NEAR(series.samples[2].accelPxPerS2, 0.0, 1e-9);
    EXPECT_NEAR(series.samples[3].accelPxPerS2, 0.0, 1e-9);

    EXPECT_DOUBLE_EQ(series.stats.totalDistancePx, 30.0);
    EXPECT_DOUBLE_EQ(series.stats.avgSpeedPxPerS, 10.0);
    EXPECT_DOUBLE_EQ(series.stats.maxSpeedPxPerS, 10.0);
}

TEST(PoseKinematics, AverageSpeedIsTimeWeightedNotSampleAveraged) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // Three tight 1s/1px hops (speed 1.0 each), then one 10s/100px hop
    // (speed 10.0) spanning a much longer real duration. An unweighted mean
    // of the 4 per-interval speeds would give (1+1+1+10)/4 = 3.25 — wrong,
    // since that treats the 10s interval as if it carried the same weight
    // as each 1s interval. The correct time-weighted average speed is
    // total distance / total duration = 103px / 13s ≈ 7.923 px/s.
    const auto result = load_frames(dir, {
        frame_json(0, 0 * kOneSecondNs,  true, 0.0,   0.0, 1.0),
        frame_json(1, 1 * kOneSecondNs,  true, 1.0,   0.0, 1.0),
        frame_json(2, 2 * kOneSecondNs,  true, 2.0,   0.0, 1.0),
        frame_json(3, 3 * kOneSecondNs,  true, 3.0,   0.0, 1.0),
        frame_json(4, 13 * kOneSecondNs, true, 103.0, 0.0, 1.0),
    });
    ASSERT_TRUE(result.is_valid());

    const KinematicsSeries series = compute_kinematics(result, 0, 0, /*smoothingWindow=*/1);

    EXPECT_DOUBLE_EQ(series.stats.totalDistancePx, 103.0);
    EXPECT_NEAR(series.stats.avgSpeedPxPerS, 103.0 / 13.0, 1e-9);
    EXPECT_DOUBLE_EQ(series.stats.maxSpeedPxPerS, 10.0);
}

TEST(PoseKinematics, LowVisibilityFrameExcludedAndGapUsesRealElapsedTime) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // Middle frame has visibility 0.05 (< 0.1 threshold) — must be skipped,
    // not interpolated. The next valid frame is 3s later (not the nominal
    // 1s spacing), so speed must be distance/3s, not distance/1s.
    const auto result = load_frames(dir, {
        frame_json(0, 0 * kOneSecondNs, true, 0.0, 0.0, 1.0),
        frame_json(1, 1 * kOneSecondNs, true, 999.0, 999.0, 0.05),
        frame_json(2, 3 * kOneSecondNs, true, 30.0, 0.0, 1.0),
    });
    ASSERT_TRUE(result.is_valid());

    const KinematicsSeries series = compute_kinematics(result, 0, 0, /*smoothingWindow=*/1);

    ASSERT_EQ(series.samples.size(), 2);  // low-visibility frame dropped
    EXPECT_TRUE(std::isnan(series.samples[0].speedPxPerS));
    EXPECT_DOUBLE_EQ(series.samples[1].speedPxPerS, 10.0);  // 30px / 3s, not / 1s
}

TEST(PoseKinematics, FrameWithNoSubjectIsExcludedLikeLowVisibility) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = load_frames(dir, {
        frame_json(0, 0 * kOneSecondNs, true, 0.0, 0.0, 1.0),
        frame_json(1, 1 * kOneSecondNs, false, 0.0, 0.0, 0.0),  // detection failed
        frame_json(2, 2 * kOneSecondNs, true, 20.0, 0.0, 1.0),
    });
    ASSERT_TRUE(result.is_valid());

    const KinematicsSeries series = compute_kinematics(result, 0, 0, 1);

    ASSERT_EQ(series.samples.size(), 2);
    EXPECT_DOUBLE_EQ(series.samples[1].speedPxPerS, 10.0);  // 20px / 2s
}

TEST(PoseKinematics, SmoothingAppliesCenteredMovingAverage) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // x positions: 0, 10, 0, 10, 0 — noisy zig-zag at constant timestamps.
    const auto result = load_frames(dir, {
        frame_json(0, 0 * kOneSecondNs, true, 0.0, 0.0, 1.0),
        frame_json(1, 1 * kOneSecondNs, true, 10.0, 0.0, 1.0),
        frame_json(2, 2 * kOneSecondNs, true, 0.0, 0.0, 1.0),
        frame_json(3, 3 * kOneSecondNs, true, 10.0, 0.0, 1.0),
        frame_json(4, 4 * kOneSecondNs, true, 0.0, 0.0, 1.0),
    });
    ASSERT_TRUE(result.is_valid());

    const KinematicsSeries series = compute_kinematics(result, 0, 0, /*smoothingWindow=*/3);

    ASSERT_EQ(series.samples.size(), 5);
    // Centered window=3 (halfWidth=1): index 0 averages [0,1] -> (0+10)/2=5
    // (clamped at the edge, only 2 samples available); index 2 averages
    // [1,2,3] -> (10+0+10)/3.
    EXPECT_DOUBLE_EQ(series.samples[0].position.x(), 5.0);
    EXPECT_NEAR(series.samples[2].position.x(), 20.0 / 3.0, 1e-9);
    EXPECT_DOUBLE_EQ(series.samples[4].position.x(), 5.0);  // clamped edge: avg of [3,4]
}

TEST(PoseKinematics, EmptyResultReturnsEmptySeriesNotCrashing) {
    const PoseAnalysisResult result;  // default-constructed, is_valid() == false
    const KinematicsSeries series = compute_kinematics(result, 0, 0, 1);

    EXPECT_TRUE(series.samples.isEmpty());
    EXPECT_DOUBLE_EQ(series.stats.totalDistancePx, 0.0);
    EXPECT_TRUE(std::isnan(series.stats.avgSpeedPxPerS));
    EXPECT_TRUE(std::isnan(series.stats.maxSpeedPxPerS));
}

TEST(PoseKinematics, SingleValidSampleHasNoDerivativesButNoCrash) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = load_frames(dir, {
        frame_json(0, 0, true, 5.0, 5.0, 1.0),
    });
    ASSERT_TRUE(result.is_valid());

    const KinematicsSeries series = compute_kinematics(result, 0, 0, 1);

    ASSERT_EQ(series.samples.size(), 1);
    EXPECT_TRUE(std::isnan(series.samples[0].speedPxPerS));
    EXPECT_TRUE(std::isnan(series.samples[0].accelPxPerS2));
    EXPECT_DOUBLE_EQ(series.stats.totalDistancePx, 0.0);
    EXPECT_TRUE(std::isnan(series.stats.avgSpeedPxPerS));
    EXPECT_TRUE(std::isnan(series.stats.maxSpeedPxPerS));
}

TEST(PoseKinematics, AllInvisibleFramesYieldEmptySamples) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = load_frames(dir, {
        frame_json(0, 0 * kOneSecondNs, true, 0.0, 0.0, 0.0),
        frame_json(1, 1 * kOneSecondNs, true, 10.0, 0.0, 0.05),
    });
    ASSERT_TRUE(result.is_valid());

    const KinematicsSeries series = compute_kinematics(result, 0, 0, 1);
    EXPECT_TRUE(series.samples.isEmpty());
}

TEST(PoseKinematics, OutOfRangeSubjectOrKeypointIndexYieldsEmptySamples) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = load_frames(dir, {
        frame_json(0, 0 * kOneSecondNs, true, 0.0, 0.0, 1.0),
        frame_json(1, 1 * kOneSecondNs, true, 10.0, 0.0, 1.0),
    });
    ASSERT_TRUE(result.is_valid());

    EXPECT_TRUE(compute_kinematics(result, /*keypointIndex=*/5, 0, 1).samples.isEmpty());
    EXPECT_TRUE(compute_kinematics(result, 0, /*subjectIndex=*/3, 1).samples.isEmpty());
}

TEST(PoseKinematics, ExactlyTwoValidSamplesGivesOneSpeedAndNoAcceleration) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = load_frames(dir, {
        frame_json(0, 0 * kOneSecondNs, true, 0.0, 0.0, 1.0),
        frame_json(1, 1 * kOneSecondNs, true, 5.0, 0.0, 1.0),
    });
    ASSERT_TRUE(result.is_valid());

    const KinematicsSeries series = compute_kinematics(result, 0, 0, 1);

    ASSERT_EQ(series.samples.size(), 2);
    EXPECT_TRUE(std::isnan(series.samples[0].speedPxPerS));
    EXPECT_DOUBLE_EQ(series.samples[1].speedPxPerS, 5.0);
    EXPECT_TRUE(std::isnan(series.samples[0].accelPxPerS2));
    EXPECT_TRUE(std::isnan(series.samples[1].accelPxPerS2));
    EXPECT_DOUBLE_EQ(series.stats.avgSpeedPxPerS, 5.0);
    EXPECT_DOUBLE_EQ(series.stats.maxSpeedPxPerS, 5.0);
}
