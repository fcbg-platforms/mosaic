#include <gtest/gtest.h>

#include "session/session_health.hpp"

using mosaic::build_session_health_report;
using mosaic::camera_health_quality_for;
using mosaic::CameraHealthInput;
using mosaic::RmsQuality;

namespace {

CameraHealthInput clean_camera(int index = 0) {
    CameraHealthInput in;
    in.index         = index;
    in.name          = QString("Camera %1").arg(index);
    in.framesGrabbed = 1000;
    in.framesEncoded = 1000;
    in.configuredFps = 25.0;
    in.achievableFps = 25.0;
    return in;
}

} // namespace

// ── camera_health_quality_for ───────────────────────────────────────────────

TEST(CameraHealthQualityFor, ExcellentForACleanCameraWithNoSyncOrTriggerData) {
    const auto raw = clean_camera();
    EXPECT_EQ(camera_health_quality_for(raw, std::nullopt), RmsQuality::Excellent);
}

TEST(CameraHealthQualityFor, PoorWheneverThereIsAnyFrameDrop) {
    auto raw          = clean_camera();
    raw.framesDropped = 1;
    EXPECT_EQ(camera_health_quality_for(raw, std::nullopt), RmsQuality::Poor);
}

TEST(CameraHealthQualityFor, PoorWheneverThereIsAnyIncompleteFrame) {
    auto raw             = clean_camera();
    raw.incompleteFrames = 1;
    EXPECT_EQ(camera_health_quality_for(raw, std::nullopt), RmsQuality::Poor);
}

TEST(CameraHealthQualityFor, PoorWhenSyncCoverageIsBelow80Percent) {
    auto raw            = clean_camera();
    raw.syncCoveragePct = 79.9;
    EXPECT_EQ(camera_health_quality_for(raw, std::nullopt), RmsQuality::Poor);
}

TEST(CameraHealthQualityFor, AcceptableWhenSyncCoverageIsBetween80And95Percent) {
    auto raw            = clean_camera();
    raw.syncCoveragePct = 94.9;
    EXPECT_EQ(camera_health_quality_for(raw, std::nullopt), RmsQuality::Acceptable);
    raw.syncCoveragePct = 80.0;
    EXPECT_EQ(camera_health_quality_for(raw, std::nullopt), RmsQuality::Acceptable);
}

TEST(CameraHealthQualityFor, ExcellentWhenSyncCoverageIsAtOrAbove95Percent) {
    auto raw            = clean_camera();
    raw.syncCoveragePct = 95.0;
    EXPECT_EQ(camera_health_quality_for(raw, std::nullopt), RmsQuality::Excellent);
}

TEST(CameraHealthQualityFor, PoorWhenMissedTriggerFramesExceedsFive) {
    const auto raw = clean_camera();
    EXPECT_EQ(camera_health_quality_for(raw, 6), RmsQuality::Poor);
}

TEST(CameraHealthQualityFor, NotPoorWhenMissedTriggerFramesIsAtOrBelowFive) {
    const auto raw = clean_camera();
    EXPECT_EQ(camera_health_quality_for(raw, 5), RmsQuality::Excellent);
    EXPECT_EQ(camera_health_quality_for(raw, 0), RmsQuality::Excellent);
}

TEST(CameraHealthQualityFor, AcceptableWhenAchievableFpsIsBelow90PercentOfConfigured) {
    auto raw          = clean_camera();
    raw.achievableFps = 22.0; // 88% of 25
    EXPECT_EQ(camera_health_quality_for(raw, std::nullopt), RmsQuality::Acceptable);
}

TEST(CameraHealthQualityFor, GoodWhenAchievableFpsIsBetween90And98PercentOfConfigured) {
    auto raw          = clean_camera();
    raw.achievableFps = 23.5; // 94% of 25
    EXPECT_EQ(camera_health_quality_for(raw, std::nullopt), RmsQuality::Good);
}

TEST(CameraHealthQualityFor, ExcellentWhenAchievableFpsHasNotBeenMeasuredYet) {
    auto raw          = clean_camera();
    raw.achievableFps = -1.0; // never measured — must not be treated as a shortfall
    EXPECT_EQ(camera_health_quality_for(raw, std::nullopt), RmsQuality::Excellent);
}

// ── build_session_health_report ─────────────────────────────────────────────

TEST(BuildSessionHealthReport, EmptyCameraListIsValidAndDoesNotCrash) {
    const auto report = build_session_health_report("path", "name", 1000, {});
    EXPECT_TRUE(report.cameras.isEmpty());
    EXPECT_EQ(report.overallQuality, RmsQuality::Excellent);
    EXPECT_FALSE(report.headline.isEmpty());
}

TEST(BuildSessionHealthReport, OverallQualityIsTheWorstAcrossAllCameras) {
    QVector<CameraHealthInput> cams;
    cams.push_back(clean_camera(0));
    auto bad          = clean_camera(1);
    bad.framesDropped = 3;
    cams.push_back(bad);
    cams.push_back(clean_camera(2));

    const auto report = build_session_health_report("path", "name", 1000, cams);
    ASSERT_EQ(report.cameras.size(), 3);
    EXPECT_EQ(report.overallQuality, RmsQuality::Poor);
    EXPECT_EQ(report.cameras[0].quality, RmsQuality::Excellent);
    EXPECT_EQ(report.cameras[1].quality, RmsQuality::Poor);
    EXPECT_EQ(report.cameras[2].quality, RmsQuality::Excellent);
}

TEST(BuildSessionHealthReport, MissedTriggerFramesIsComputedAndClampedToZero) {
    QVector<CameraHealthInput> cams;
    auto normal             = clean_camera(0);
    normal.actionTicksFired = 1010;
    normal.framesGrabbed    = 1000;
    cams.push_back(normal);

    // A camera can legitimately have framesGrabbed slightly ahead of
    // ticksFired from residual free-run frames captured before the ticker's
    // first tick — must clamp to 0, never go negative.
    auto aheadOfTicks             = clean_camera(1);
    aheadOfTicks.actionTicksFired = 5;
    aheadOfTicks.framesGrabbed    = 8;
    cams.push_back(aheadOfTicks);

    const auto report = build_session_health_report("path", "name", 1000, cams);
    ASSERT_EQ(report.cameras.size(), 2);
    ASSERT_TRUE(report.cameras[0].missedTriggerFrames.has_value());
    EXPECT_EQ(*report.cameras[0].missedTriggerFrames, 10);
    ASSERT_TRUE(report.cameras[1].missedTriggerFrames.has_value());
    EXPECT_EQ(*report.cameras[1].missedTriggerFrames, 0);
}

TEST(BuildSessionHealthReport, MissedTriggerFramesIsNulloptWhenActionCommandWasNotUsed) {
    QVector<CameraHealthInput> cams;
    cams.push_back(clean_camera(0));

    const auto report = build_session_health_report("path", "name", 1000, cams);
    ASSERT_EQ(report.cameras.size(), 1);
    EXPECT_FALSE(report.cameras[0].missedTriggerFrames.has_value());
}

TEST(BuildSessionHealthReport, HeadlineNamesTheCleanCameraCountWhenAllAreClean) {
    QVector<CameraHealthInput> cams;
    cams.push_back(clean_camera(0));
    cams.push_back(clean_camera(1));

    const auto report = build_session_health_report("path", "name", 1000, cams);
    EXPECT_EQ(report.headline, "2/2 cameras clean");
}
