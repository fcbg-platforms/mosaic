#include <gtest/gtest.h>

#include "video/fps_readout.hpp"

using mosaic::compute_fps_readout;
using mosaic::FpsReadoutKind;
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
