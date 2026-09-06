#include <gtest/gtest.h>

#include "video/fps_readout.hpp"

using mosaic::compute_fps_readout;
using mosaic::FpsLimit;
using mosaic::FpsReadoutKind;
using mosaic::k_fps_at_cap_tolerance;
using mosaic::k_fps_shortfall_factor;

namespace {

// The camera's own measurement, whenever there is one, always wins over
// anything derivable from the settings — it is the only figure that accounts
// for exposure, sensor readout and link bandwidth together.
constexpr double kNoMeasurement = -1.0;

} // namespace

TEST(FpsReadout, AMeasurementIsPreferredOverTheExposureCeiling) {
    // Manual exposure of 10 ms would imply a 100 fps ceiling, but the camera
    // reports 23.4 — bandwidth or readout, not exposure, is the real limit.
    const auto r = compute_fps_readout(23.4, /*specifyFps=*/true, /*configuredFps=*/25.0,
                                       /*manualExposure=*/true, /*exposureTimeUs=*/10'000.0);
    EXPECT_EQ(r.kind, FpsReadoutKind::Measured);
    EXPECT_DOUBLE_EQ(r.fps, 23.4);
    // 23.4 is above 25 * 0.9 == 22.5, so this is not a shortfall.
    EXPECT_FALSE(r.belowConfigured);
}

TEST(FpsReadout, AMeasurementBelowTheShortfallThresholdIsFlagged) {
    const auto r = compute_fps_readout(15.7, true, 25.0, false, 10'000.0);
    EXPECT_EQ(r.kind, FpsReadoutKind::Measured);
    EXPECT_TRUE(r.belowConfigured);
}

// The threshold is shared with VideoGrabber::refresh_achievable_fps()'s own
// log warning, so pin both sides of it exactly: a reading sitting right on
// the boundary must not be called a shortfall.
TEST(FpsReadout, ShortfallBoundaryMatchesTheSharedThreshold) {
    const double configured = 25.0;
    const double onTheLine  = configured * k_fps_shortfall_factor; // 22.5

    EXPECT_FALSE(compute_fps_readout(onTheLine, true, configured, false, 0.0).belowConfigured);
    EXPECT_TRUE(
        compute_fps_readout(onTheLine - 0.01, true, configured, false, 0.0).belowConfigured);
}

// Without a pinned target rate there is nothing to fall short of, however low
// the measurement is.
TEST(FpsReadout, NothingFallsShortWhenNoFrameRateIsSpecified) {
    const auto r = compute_fps_readout(2.0, /*specifyFps=*/false, 25.0, false, 0.0);
    EXPECT_EQ(r.kind, FpsReadoutKind::Measured);
    EXPECT_FALSE(r.belowConfigured);
}

TEST(FpsReadout, ManualExposureWithoutAMeasurementGivesTheExposureCeiling) {
    // 40 ms exposure cannot yield more than 25 fps.
    const auto r = compute_fps_readout(kNoMeasurement, true, 25.0,
                                       /*manualExposure=*/true, /*exposureTimeUs=*/40'000.0);
    EXPECT_EQ(r.kind, FpsReadoutKind::ExposureCeiling);
    EXPECT_DOUBLE_EQ(r.fps, 25.0);
    EXPECT_FALSE(r.belowConfigured);
}

// The one real problem the ceiling can catch before a camera is ever opened:
// an exposure time that alone rules out the requested rate.
TEST(FpsReadout, AnExposureCeilingBelowTheConfiguredRateIsFlagged) {
    const auto r = compute_fps_readout(kNoMeasurement, true, 25.0, true,
                                       /*exposureTimeUs=*/100'000.0); // 10 fps ceiling
    EXPECT_EQ(r.kind, FpsReadoutKind::ExposureCeiling);
    EXPECT_DOUBLE_EQ(r.fps, 10.0);
    EXPECT_TRUE(r.belowConfigured);
}

// Under auto exposure the camera chooses its own exposure time when it opens,
// so exposureTimeUs describes nothing that will actually happen. Reporting a
// ceiling from it would be a confidently wrong number, which is exactly what
// this module exists to avoid.
TEST(FpsReadout, AutoExposureWithoutAMeasurementReportsNoNumberAtAll) {
    const auto r = compute_fps_readout(kNoMeasurement, true, 25.0,
                                       /*manualExposure=*/false, /*exposureTimeUs=*/40'000.0);
    EXPECT_EQ(r.kind, FpsReadoutKind::AwaitingMeasurement);
    EXPECT_LT(r.fps, 0.0);
    EXPECT_FALSE(r.belowConfigured);
}

// achievable_fps() documents -1.0 as "never measured", but a stub build or an
// unavailable node could equally yield 0 — neither is a rate.
TEST(FpsReadout, ZeroAndNegativeMeasurementsAreBothTreatedAsNoMeasurement) {
    for (const double measured : {0.0, -1.0, -42.0}) {
        const auto r = compute_fps_readout(measured, true, 25.0, false, 40'000.0);
        EXPECT_EQ(r.kind, FpsReadoutKind::AwaitingMeasurement) << "measured = " << measured;
    }
}

// A zero/garbage exposure would divide to infinity; fall through to the
// honest "nothing to report" state instead.
TEST(FpsReadout, ANonPositiveExposureTimeYieldsNoCeiling) {
    const auto r = compute_fps_readout(kNoMeasurement, true, 25.0, true, 0.0);
    EXPECT_EQ(r.kind, FpsReadoutKind::AwaitingMeasurement);
}

// ── Which constraint is binding ────────────────────────────────────────────
//
// The readout showing a correct-but-motionless number is the failure these
// cover: a camera pinned to 15 fps reports 15 fps however the exposure
// control is moved, and without naming the cap the UI looks broken.

TEST(FpsReadoutLimit, AMeasurementAtTheConfiguredRateIsCappedByIt) {
    const auto r = compute_fps_readout(25.0, /*specifyFps=*/true, /*configuredFps=*/25.0,
                                       /*manualExposure=*/true, /*exposureTimeUs=*/10'000.0);
    EXPECT_EQ(r.limitedBy, FpsLimit::ConfiguredRate);
    EXPECT_FALSE(r.belowConfigured);
    // 1e6 / 25 == 40 ms: below that, exposure cannot be what limits the rate.
    EXPECT_DOUBLE_EQ(r.exposureCrossoverUs, 40'000.0);
}

// The reading that prompted this change: every acA1920-25gc in room 11 logged
// resultFPS=14.9999 against a configured 15, so the tolerance has to absorb
// the camera's own rounding rather than calling this a shortfall.
TEST(FpsReadoutLimit, TheRealRoom11ReadingCountsAsAtTheCap) {
    const auto r = compute_fps_readout(14.9999, true, 15.0, true, 10'000.0);
    EXPECT_EQ(r.kind, FpsReadoutKind::Measured);
    EXPECT_EQ(r.limitedBy, FpsLimit::ConfiguredRate);
    EXPECT_NEAR(r.exposureCrossoverUs, 66'666.7, 0.1);
}

// Pin both sides of the cap tolerance, the same way the shortfall boundary is
// pinned above.
TEST(FpsReadoutLimit, CapBoundaryMatchesTheSharedTolerance) {
    const double configured = 25.0;
    const double onTheLine  = configured * (1.0 - k_fps_at_cap_tolerance); // 24.75

    EXPECT_EQ(compute_fps_readout(onTheLine, true, configured, false, 0.0).limitedBy,
              FpsLimit::ConfiguredRate);
    EXPECT_EQ(compute_fps_readout(onTheLine - 0.01, true, configured, false, 0.0).limitedBy,
              FpsLimit::Other);
}

// Between the two thresholds the camera is under its cap but not by enough to
// warn about — still "something other than the cap is binding".
TEST(FpsReadoutLimit, AMeasurementUnderTheCapButAboveTheShortfallIsOther) {
    const auto r = compute_fps_readout(24.0, true, 25.0, false, 0.0); // 96% of configured
    EXPECT_EQ(r.limitedBy, FpsLimit::Other);
    EXPECT_FALSE(r.belowConfigured);
    EXPECT_LT(r.exposureCrossoverUs, 0.0);
}

// The two classifications are defined so they can never both fire; the label
// code relies on that to avoid contradicting itself.
TEST(FpsReadoutLimit, AShortfallIsOtherAndNeverAlsoAtTheCap) {
    const auto r = compute_fps_readout(15.7, true, 25.0, false, 10'000.0);
    EXPECT_TRUE(r.belowConfigured);
    EXPECT_EQ(r.limitedBy, FpsLimit::Other);
    EXPECT_LT(r.exposureCrossoverUs, 0.0);
}

// A free-running camera has no cap to be at, however fast it happens to run.
TEST(FpsReadoutLimit, WithoutASpecifiedRateNothingCanBeClassified) {
    const auto r = compute_fps_readout(200.0, /*specifyFps=*/false, 25.0, true, 1'000.0);
    EXPECT_EQ(r.kind, FpsReadoutKind::Measured);
    EXPECT_EQ(r.limitedBy, FpsLimit::Unknown);
    EXPECT_LT(r.exposureCrossoverUs, 0.0);
}

// Neither measurement-free state gets classified: ExposureCeiling's own
// wording already names exposure as the limit, and AwaitingMeasurement has
// nothing to reason from at all.
TEST(FpsReadoutLimit, StatesWithoutAMeasurementAreAlwaysUnknown) {
    const auto ceiling = compute_fps_readout(-1.0, true, 25.0, true, 40'000.0);
    EXPECT_EQ(ceiling.kind, FpsReadoutKind::ExposureCeiling);
    EXPECT_EQ(ceiling.limitedBy, FpsLimit::Unknown);

    const auto awaiting = compute_fps_readout(-1.0, true, 25.0, false, 40'000.0);
    EXPECT_EQ(awaiting.kind, FpsReadoutKind::AwaitingMeasurement);
    EXPECT_EQ(awaiting.limitedBy, FpsLimit::Unknown);
}

// The frame-rate spinbox relabels synchronously on every valueChanged while
// the last measurement still describes the *previous* target, so dragging
// 30 fps down to 15 transiently pairs a 30 fps reading with a 15 fps cap.
// Calling that "capped by the 15 fps setting" would assert a cap the number
// on screen visibly doubles.
TEST(FpsReadoutLimit, AMeasurementAboveTheConfiguredRateIsNotCappedByIt) {
    const auto r = compute_fps_readout(30.0, /*specifyFps=*/true, /*configuredFps=*/15.0,
                                       /*manualExposure=*/true, /*exposureTimeUs=*/10'000.0);
    EXPECT_EQ(r.kind, FpsReadoutKind::Measured);
    EXPECT_EQ(r.limitedBy, FpsLimit::Unknown);
    EXPECT_FALSE(r.belowConfigured);
    EXPECT_LT(r.exposureCrossoverUs, 0.0);
}

// The cap band is two-sided: pin the upper edge the same way the lower one is
// pinned, so a camera overshooting slightly still reads as "at the cap".
TEST(FpsReadoutLimit, CapBandUpperEdgeIsAlsoBoundedByTheTolerance) {
    const double configured = 25.0;
    const double justOver   = configured * (1.0 + k_fps_at_cap_tolerance); // 25.25

    EXPECT_EQ(compute_fps_readout(justOver, true, configured, false, 0.0).limitedBy,
              FpsLimit::ConfiguredRate);
    EXPECT_EQ(compute_fps_readout(justOver + 0.01, true, configured, false, 0.0).limitedBy,
              FpsLimit::Unknown);
}
