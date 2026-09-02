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

TEST(CameraHealthQualityFor, PoorWhenCameraNeverParticipated) {
    // Configured but never opened — duplicate serial, failed open(), or a dead
    // cable/NIC link. Every counter stays at its default, so before this rule
    // existed such a camera fell through every check and graded Excellent,
    // outranking cameras that actually recorded successfully.
    CameraHealthInput raw;
    raw.index        = 5;
    raw.name         = "Camera 6 (24893039)";
    raw.participated = false;
    EXPECT_EQ(camera_health_quality_for(raw, std::nullopt), RmsQuality::Poor);
}

TEST(CameraHealthQualityFor, PoorWhenNoFramesWereGrabbedAtAll) {
    // A camera that failed to open, or whose link was down for the whole
    // session, has no drops, no incomplete frames, no sync data, and an
    // unmeasured achievableFps — so before this check existed it fell through
    // every other rule and graded Excellent, which is exactly backwards.
    CameraHealthInput raw;
    raw.index         = 5;
    raw.name          = "Camera 5";
    raw.framesGrabbed = 0;
    raw.framesEncoded = 0;
    raw.configuredFps = 25.0;
    EXPECT_EQ(camera_health_quality_for(raw, std::nullopt), RmsQuality::Poor);

    // Still Poor even when sync/trigger data happens to look fine.
    raw.syncCoveragePct = 100.0;
    EXPECT_EQ(camera_health_quality_for(raw, 0), RmsQuality::Poor);
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

TEST(BuildSessionHealthReport, ANonParticipatingCameraDominatesTheOverallVerdict) {
    // The whole point of reporting a never-opened camera at all: two healthy
    // cameras must not let a dead one pass unnoticed.
    QVector<CameraHealthInput> cams;
    cams.push_back(clean_camera(0));
    cams.push_back(clean_camera(1));
    CameraHealthInput dead;
    dead.index        = 5;
    dead.name         = "Camera 6 (24893039)";
    dead.participated = false;
    cams.push_back(dead);

    const auto report = build_session_health_report("path", "name", 1000, cams);
    EXPECT_EQ(report.overallQuality, RmsQuality::Poor);
    EXPECT_EQ(report.cameras.size(), 3);
    EXPECT_EQ(report.cameras[2].quality, RmsQuality::Poor);
    EXPECT_TRUE(report.headline.contains("Camera 6 (24893039)"));
    EXPECT_TRUE(report.headline.contains("Poor"));
}

TEST(BuildSessionHealthReport, HeadlineFallbackNameIsOneBasedLikeTheProducerLabel) {
    // A caller that leaves `name` empty must not produce "Camera 0" for the
    // same camera the producer would call "Camera 1".
    QVector<CameraHealthInput> cams;
    CameraHealthInput bad;
    bad.index         = 0;
    bad.framesGrabbed = 1000;
    bad.framesDropped = 3;
    cams.push_back(bad);

    const auto report = build_session_health_report("path", "name", 1000, cams);
    EXPECT_TRUE(report.headline.startsWith("Camera 1:"));
}
