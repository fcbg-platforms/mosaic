#include "analysis/realtime_metrics.hpp"
#include <gtest/gtest.h>

using mosaic::DetectionRateTracker;
using mosaic::RmsQuality;
using mosaic::gaze_on_target_for;
using mosaic::kGazeOnTargetThreshold;
using mosaic::pose_tracking_quality_for;
using mosaic::rppg_quality_for;

// ── pose_tracking_quality_for ───────────────────────────────────────────────

TEST(PoseTrackingQualityFor, ExcellentAt0Point85AndAbove) {
    EXPECT_EQ(pose_tracking_quality_for(0.85), RmsQuality::Excellent);
    EXPECT_EQ(pose_tracking_quality_for(1.0), RmsQuality::Excellent);
}

TEST(PoseTrackingQualityFor, GoodBetween0Point6And0Point85) {
    EXPECT_EQ(pose_tracking_quality_for(0.60), RmsQuality::Good);
    EXPECT_EQ(pose_tracking_quality_for(0.84), RmsQuality::Good);
}

TEST(PoseTrackingQualityFor, AcceptableBetween0Point3And0Point6) {
    EXPECT_EQ(pose_tracking_quality_for(0.30), RmsQuality::Acceptable);
    EXPECT_EQ(pose_tracking_quality_for(0.59), RmsQuality::Acceptable);
}

TEST(PoseTrackingQualityFor, PoorBelow0Point3) {
    EXPECT_EQ(pose_tracking_quality_for(0.29), RmsQuality::Poor);
    EXPECT_EQ(pose_tracking_quality_for(0.0), RmsQuality::Poor);
}

// ── gaze_on_target_for ──────────────────────────────────────────────────────

TEST(GazeOnTargetFor, TrueAtOrigin) {
    EXPECT_TRUE(gaze_on_target_for(0.0, 0.0));
}

TEST(GazeOnTargetFor, TrueExactlyAtThresholdRadius) {
    EXPECT_TRUE(gaze_on_target_for(kGazeOnTargetThreshold, 0.0));
    EXPECT_TRUE(gaze_on_target_for(0.0, kGazeOnTargetThreshold));
}

TEST(GazeOnTargetFor, FalseJustOutsideThresholdRadius) {
    EXPECT_FALSE(gaze_on_target_for(kGazeOnTargetThreshold + 0.01, 0.0));
}

TEST(GazeOnTargetFor, FalseAtClampExtremes) {
    EXPECT_FALSE(gaze_on_target_for(1.0, 1.0));
    EXPECT_FALSE(gaze_on_target_for(-1.0, -1.0));
}

TEST(GazeOnTargetFor, RespectsCustomThreshold) {
    EXPECT_TRUE(gaze_on_target_for(0.5, 0.0, /*threshold=*/0.6));
    EXPECT_FALSE(gaze_on_target_for(0.5, 0.0, /*threshold=*/0.4));
}

// ── DetectionRateTracker ────────────────────────────────────────────────────

TEST(DetectionRateTracker, EmptyTrackerReportsNoData) {
    DetectionRateTracker t;
    EXPECT_FALSE(t.rate().has_value());
    EXPECT_TRUE(t.bucket_rates().isEmpty());
    EXPECT_EQ(t.bucket_count(), 0);
}

TEST(DetectionRateTracker, SingleBucketAggregatesHitsOverTotal) {
    DetectionRateTracker t(24, 5000);
    // All within the same 5s bucket (timestamps 0, 1000, 2000, 3000 ms).
    t.push(true, 0);
    t.push(true, 1000);
    t.push(false, 2000);
    t.push(true, 3000);

    ASSERT_TRUE(t.rate().has_value());
    EXPECT_NEAR(*t.rate(), 0.75, 1e-9);
    EXPECT_EQ(t.bucket_count(), 1);
}

TEST(DetectionRateTracker, RollsForwardIntoNewBucketsAcrossTime) {
    DetectionRateTracker t(24, 5000);
    t.push(true, 0);       // bucket 0
    t.push(true, 5000);    // bucket 1
    t.push(false, 10000);  // bucket 2

    EXPECT_EQ(t.bucket_count(), 3);
    ASSERT_TRUE(t.rate().has_value());
    EXPECT_NEAR(*t.rate(), 2.0 / 3.0, 1e-9);

    const auto rates = t.bucket_rates();
    ASSERT_EQ(rates.size(), 3);
    EXPECT_NEAR(rates[0], 1.0, 1e-9);
    EXPECT_NEAR(rates[1], 1.0, 1e-9);
    EXPECT_NEAR(rates[2], 0.0, 1e-9);
}

TEST(DetectionRateTracker, EvictsOldestBucketPastCapacity) {
    DetectionRateTracker t(/*bucketCount=*/2, /*bucketDurationMs=*/1000);
    t.push(false, 0);      // bucket 0 — should be evicted
    t.push(true, 1000);    // bucket 1
    t.push(true, 2000);    // bucket 2

    EXPECT_EQ(t.bucket_count(), 2);
    ASSERT_TRUE(t.rate().has_value());
    // Only buckets 1 and 2 remain, both all-true.
    EXPECT_NEAR(*t.rate(), 1.0, 1e-9);
}

TEST(DetectionRateTracker, IgnoresOutOfOrderTimestamp) {
    DetectionRateTracker t(24, 1000);
    t.push(true, 5000);
    t.push(false, 1000);   // stale, out of order — dropped
    ASSERT_TRUE(t.rate().has_value());
    EXPECT_NEAR(*t.rate(), 1.0, 1e-9);
}

// ── rppg_quality_for ─────────────────────────────────────────────────────────

TEST(RppgQualityFor, LowValidFrameFractionForcesPoorRegardlessOfSnr) {
    // A quality read computed on sparse/mostly-missing face data isn't
    // meaningful, even if the reported SNR number happens to look high.
    EXPECT_EQ(rppg_quality_for(/*snrDb=*/20.0, /*validFrameFraction=*/0.59), RmsQuality::Poor);
    EXPECT_EQ(rppg_quality_for(20.0, 0.0), RmsQuality::Poor);
}

TEST(RppgQualityFor, ExcellentAt5DbAndAboveWithSufficientValidFraction) {
    EXPECT_EQ(rppg_quality_for(5.0, 0.6), RmsQuality::Excellent);
    EXPECT_EQ(rppg_quality_for(20.0, 1.0), RmsQuality::Excellent);
}

TEST(RppgQualityFor, GoodBetween0And5Db) {
    EXPECT_EQ(rppg_quality_for(0.0, 0.6), RmsQuality::Good);
    EXPECT_EQ(rppg_quality_for(4.9, 0.9), RmsQuality::Good);
}

TEST(RppgQualityFor, AcceptableBetweenNegative5And0Db) {
    EXPECT_EQ(rppg_quality_for(-5.0, 0.6), RmsQuality::Acceptable);
    EXPECT_EQ(rppg_quality_for(-0.1, 0.9), RmsQuality::Acceptable);
}

TEST(RppgQualityFor, PoorBelowNegative5Db) {
    EXPECT_EQ(rppg_quality_for(-5.1, 1.0), RmsQuality::Poor);
    EXPECT_EQ(rppg_quality_for(-60.0, 1.0), RmsQuality::Poor);
}
