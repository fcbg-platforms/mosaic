#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "video/camera_health.hpp"

using mosaic::CameraHealth;
using mosaic::classify_camera_health;
using mosaic::classify_cameras;
using mosaic::peer_median_fps;

// ── peer_median_fps ────────────────────────────────────────────────────────

TEST(PeerMedianFps, NothingRunningHasNoBaseline) { EXPECT_DOUBLE_EQ(peer_median_fps({}), 0.0); }

TEST(PeerMedianFps, OddAndEvenCounts) {
    EXPECT_DOUBLE_EQ(peer_median_fps({14.0, 15.0, 16.0}), 15.0);
    EXPECT_DOUBLE_EQ(peer_median_fps({14.0, 15.0, 16.0, 17.0}), 15.5);
    EXPECT_DOUBLE_EQ(peer_median_fps({14.4}), 14.4);
}

TEST(PeerMedianFps, DoesNotCareAboutInputOrder) {
    EXPECT_DOUBLE_EQ(peer_median_fps({16.0, 14.0, 15.0}), 15.0);
    EXPECT_DOUBLE_EQ(peer_median_fps({17.0, 14.0, 16.0, 15.0}), 15.5);
}

// The reason it is a median and not a mean, in the exact shape this rig
// produced it: five cameras, two collapsed. The mean is dragged to 10.3, which
// would make the two failures look merely below-average; the median stays on
// what a healthy camera is actually managing.
TEST(PeerMedianFps, TwoCollapsedCamerasDoNotDragTheBaselineDown) {
    const std::vector<double> observed{14.4, 15.1, 5.3, 2.3, 14.4};
    EXPECT_DOUBLE_EQ(peer_median_fps(observed), 14.4);

    double mean = 0.0;
    for (const double v : observed) mean += v;
    mean /= static_cast<double>(observed.size());
    EXPECT_LT(mean, 11.0); // the number we are deliberately not using
}

// ── classify_camera_health ─────────────────────────────────────────────────

// The case this exists for, with the real numbers off
// sub-01_ses-02_task-rest_run-01_20260907T095912.
TEST(CameraHealthClassify, TheObservedFailureReadsAsStalled) {
    const double median = peer_median_fps({14.4, 15.1, 5.3, 2.3, 14.4});
    EXPECT_EQ(classify_camera_health(14.4, median, true), CameraHealth::Ok);
    EXPECT_EQ(classify_camera_health(15.1, median, true), CameraHealth::Ok);
    EXPECT_EQ(classify_camera_health(5.3, median, true), CameraHealth::Stalled);
    EXPECT_EQ(classify_camera_health(2.3, median, true), CameraHealth::Stalled);
}

// The whole point of comparing against peers rather than the configured rate.
// For months every camera on this rig ran at ~14.4 fps against 25 configured,
// and none of them was faulty — a configured-rate threshold would have flagged
// all five, permanently.
TEST(CameraHealthClassify, AUniformlyBandwidthLimitedRigIsHealthy) {
    const double median = peer_median_fps({14.4, 14.5, 14.4, 14.4, 14.3});
    for (const double fps : {14.4, 14.5, 14.4, 14.4, 14.3}) {
        EXPECT_EQ(classify_camera_health(fps, median, true), CameraHealth::Ok)
            << "fps " << fps << " against median " << median;
    }
}

TEST(CameraHealthClassify, ThresholdsSitWhereTheConstantsSay) {
    constexpr double median = 10.0;
    EXPECT_EQ(classify_camera_health(7.1, median, true), CameraHealth::Ok);
    EXPECT_EQ(classify_camera_health(6.9, median, true), CameraHealth::Lagging);
    EXPECT_EQ(classify_camera_health(4.1, median, true), CameraHealth::Lagging);
    EXPECT_EQ(classify_camera_health(3.9, median, true), CameraHealth::Stalled);
    EXPECT_EQ(classify_camera_health(0.0, median, true), CameraHealth::Stalled);
}

// A camera that is not grabbing is switched off, not broken. Reporting a fault
// for it would be crying wolf about the ordinary case of a rig running four of
// its six cameras.
TEST(CameraHealthClassify, ANonRunningCameraIsUnknownNotStalled) {
    EXPECT_EQ(classify_camera_health(0.0, 14.4, false), CameraHealth::Unknown);
    EXPECT_EQ(classify_camera_health(14.4, 14.4, false), CameraHealth::Unknown);
}

// With no healthy baseline there is nothing to compare against, and inventing
// one from the configured rate would bring back the false alarms this design
// exists to avoid.
TEST(CameraHealthClassify, NoBaselineMeansUnknown) {
    EXPECT_EQ(classify_camera_health(14.4, 0.0, true), CameraHealth::Unknown);
    EXPECT_EQ(classify_camera_health(0.0, 0.0, true), CameraHealth::Unknown);
    EXPECT_EQ(classify_camera_health(14.4, std::nan(""), true), CameraHealth::Unknown);
}

// Measured against itself a camera is always exactly at the baseline. That is
// why callers must use classify_cameras(), which excludes self — see below.
TEST(CameraHealthClassify, AgainstItselfACameraIsAlwaysOk) {
    EXPECT_EQ(classify_camera_health(14.4, peer_median_fps({14.4}), true), CameraHealth::Ok);
    EXPECT_EQ(classify_camera_health(0.4, peer_median_fps({0.4}), true), CameraHealth::Ok);
}

// ── classify_cameras ───────────────────────────────────────────────────────

TEST(ClassifyCameras, TheFiveCameraFailureThatPromptedThis) {
    const auto out = classify_cameras({14.4, 15.1, 5.3, 2.3, 14.4}, {true, true, true, true, true});
    ASSERT_EQ(out.size(), 5u);
    EXPECT_EQ(out[0], CameraHealth::Ok);
    EXPECT_EQ(out[1], CameraHealth::Ok);
    EXPECT_EQ(out[2], CameraHealth::Stalled);
    EXPECT_EQ(out[3], CameraHealth::Stalled);
    EXPECT_EQ(out[4], CameraHealth::Ok);
}

// The case a single shared median gets wrong, and the reason self is excluded.
// Combined median of these four is (5.3 + 14.4)/2 = 9.85 — dragged halfway to
// the failures by the failures — and 5.3/9.85 = 0.54 would read as merely
// Lagging. Excluding self, the 5.3 camera's peers are 14.4/14.4/2.3, median
// 14.4, ratio 0.37: Stalled, which is what it is.
TEST(ClassifyCameras, EvenCountWithHalfTheRigDegraded) {
    const auto out = classify_cameras({14.4, 14.4, 5.3, 2.3}, {true, true, true, true});
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], CameraHealth::Ok);
    EXPECT_EQ(out[1], CameraHealth::Ok);
    EXPECT_EQ(out[2], CameraHealth::Stalled);
    EXPECT_EQ(out[3], CameraHealth::Stalled);

    // The shared-median answer this deliberately does not give.
    EXPECT_DOUBLE_EQ(peer_median_fps({14.4, 14.4, 5.3, 2.3}), 9.85);
    EXPECT_EQ(classify_camera_health(5.3, 9.85, true), CameraHealth::Lagging);
}

TEST(ClassifyCameras, AUniformlyBandwidthLimitedRigIsAllOk) {
    const auto out =
        classify_cameras({14.4, 14.5, 14.4, 14.4, 14.3}, {true, true, true, true, true});
    for (const auto h : out) {
        EXPECT_EQ(h, CameraHealth::Ok);
    }
}

// A camera that is not grabbing reports Unknown, and is kept out of everyone
// else's baseline — a stopped camera contributing 0 fps would drag the median
// down and start excusing genuinely failing cameras.
TEST(ClassifyCameras, StoppedCamerasAreExcludedFromTheBaseline) {
    const auto out = classify_cameras({14.4, 14.4, 0.0, 5.3}, {true, true, false, true});
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], CameraHealth::Ok);
    EXPECT_EQ(out[1], CameraHealth::Ok);
    EXPECT_EQ(out[2], CameraHealth::Unknown); // stopped, not broken
    EXPECT_EQ(out[3], CameraHealth::Stalled); // judged against 14.4, not against 0
}

// One camera has no peers, so there is genuinely nothing to compare it against
// and it reports Unknown rather than inventing a verdict.
TEST(ClassifyCameras, ALoneCameraCannotBeJudged) {
    EXPECT_EQ(classify_cameras({14.4}, {true}).at(0), CameraHealth::Unknown);
    EXPECT_EQ(classify_cameras({0.1}, {true}).at(0), CameraHealth::Unknown);
}

TEST(ClassifyCameras, NoCamerasIsEmptyNotACrash) { EXPECT_TRUE(classify_cameras({}, {}).empty()); }

// The running vector being short must not read out of bounds; those cameras
// are treated as not running.
TEST(ClassifyCameras, ShorterRunningVectorIsSafe) {
    const auto out = classify_cameras({14.4, 14.4, 5.3}, {true, true});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[2], CameraHealth::Unknown);
}

// A camera running *faster* than its peers is not a fault.
TEST(CameraHealthClassify, RunningAheadOfThePeersIsFine) {
    EXPECT_EQ(classify_camera_health(25.0, 14.4, true), CameraHealth::Ok);
}

// ── delivered_fps ──────────────────────────────────────────────────────────

// The grabber only recomputes its rate when a good frame arrives, so a camera
// whose link has dropped keeps reporting its last healthy rate. Judged by that
// number, an unplugged camera would read Ok for as long as it stayed unplugged.
TEST(DeliveredFps, AStoppedCameraReadsZeroWhateverItLastMeasured) {
    EXPECT_DOUBLE_EQ(mosaic::delivered_fps(25.0, 0.04), 25.0);
    EXPECT_DOUBLE_EQ(mosaic::delivered_fps(25.0, mosaic::k_camera_stale_frame_sec), 25.0);
    EXPECT_DOUBLE_EQ(mosaic::delivered_fps(25.0, mosaic::k_camera_stale_frame_sec + 0.1), 0.0);
    EXPECT_DOUBLE_EQ(mosaic::delivered_fps(25.0, 600.0), 0.0);
    EXPECT_DOUBLE_EQ(mosaic::delivered_fps(25.0, -1.0), 0.0); // never delivered
    EXPECT_DOUBLE_EQ(mosaic::delivered_fps(-3.0, 0.1), 0.0);
}

// End to end with the classifier: one camera unplugged among five healthy ones
// goes red, which is what the Live tab's chip must show.
TEST(DeliveredFps, AnUnpluggedCameraAmongHealthyPeersIsStalled) {
    const std::vector<double> fps{
        mosaic::delivered_fps(24.9, 0.03), mosaic::delivered_fps(25.0, 0.05),
        mosaic::delivered_fps(25.0, 9.0), // unplugged 9 s ago, still "25.0"
        mosaic::delivered_fps(24.8, 0.02), mosaic::delivered_fps(25.0, 0.04)};
    const auto states = classify_cameras(fps, {true, true, true, true, true});
    EXPECT_EQ(states[2], CameraHealth::Stalled);
    EXPECT_EQ(states[0], CameraHealth::Ok);
}
