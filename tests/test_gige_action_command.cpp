#include "video/gige_action_command.hpp"
#include <gtest/gtest.h>
#include <vector>

using mosaic::action_command_period_ms;
using mosaic::ipv4_broadcast_address;
using mosaic::ipv4_from_dotted;
using mosaic::ipv4_to_dotted;
using mosaic::is_achievable_fps_measurement_warmed_up;
using mosaic::k_default_action_fps;
using mosaic::k_default_action_margin;
using mosaic::k_default_fps_warmup_seconds;

TEST(Ipv4BroadcastAddress, ClassCSubnet) {
    // The real room-11 scenario: each camera on its own /24.
    EXPECT_EQ(ipv4_broadcast_address(*ipv4_from_dotted("192.168.3.42"),
                                      *ipv4_from_dotted("255.255.255.0")),
              *ipv4_from_dotted("192.168.3.255"));
}

TEST(Ipv4BroadcastAddress, ClassBSubnet) {
    EXPECT_EQ(ipv4_broadcast_address(*ipv4_from_dotted("192.168.3.42"),
                                      *ipv4_from_dotted("255.255.0.0")),
              *ipv4_from_dotted("192.168.255.255"));
}

TEST(Ipv4BroadcastAddress, HostMaskYieldsSelf) {
    // /32 — a single-host mask; broadcast address degenerates to the host's own IP.
    const auto ip = *ipv4_from_dotted("10.0.0.5");
    EXPECT_EQ(ipv4_broadcast_address(ip, *ipv4_from_dotted("255.255.255.255")), ip);
}

TEST(Ipv4BroadcastAddress, ZeroMaskYieldsGlobalBroadcast) {
    EXPECT_EQ(ipv4_broadcast_address(*ipv4_from_dotted("10.0.0.5"),
                                      *ipv4_from_dotted("0.0.0.0")),
              *ipv4_from_dotted("255.255.255.255"));
}

TEST(Ipv4FromDotted, ParsesValidAddress) {
    ASSERT_TRUE(ipv4_from_dotted("192.168.3.42").has_value());
    EXPECT_EQ(ipv4_to_dotted(*ipv4_from_dotted("192.168.3.42")), "192.168.3.42");
}

TEST(Ipv4FromDotted, RejectsTooFewOctets) {
    EXPECT_FALSE(ipv4_from_dotted("192.168.3").has_value());
}

TEST(Ipv4FromDotted, RejectsTooManyOctets) {
    EXPECT_FALSE(ipv4_from_dotted("192.168.3.42.1").has_value());
}

TEST(Ipv4FromDotted, RejectsOutOfRangeOctet) {
    // 256 is not a valid octet — this is exactly the kind of malformed
    // GevCurrentIPAddress readback (e.g. a camera in an unexpected IP
    // configuration state) that must fail closed, not silently produce a
    // garbage broadcast address.
    EXPECT_FALSE(ipv4_from_dotted("192.168.3.256").has_value());
}

TEST(Ipv4FromDotted, RejectsNonNumeric) {
    EXPECT_FALSE(ipv4_from_dotted("abc.def.gh.i").has_value());
}

TEST(Ipv4FromDotted, RejectsEmptyString) {
    EXPECT_FALSE(ipv4_from_dotted("").has_value());
}

TEST(Ipv4ToDotted, RoundTripsThroughFromDotted) {
    for (const QString& addr : {QStringLiteral("0.0.0.0"), QStringLiteral("255.255.255.255"),
                                 QStringLiteral("192.168.3.42"), QStringLiteral("10.0.0.1")}) {
        ASSERT_TRUE(ipv4_from_dotted(addr).has_value());
        EXPECT_EQ(ipv4_to_dotted(*ipv4_from_dotted(addr)), addr);
    }
}

TEST(ActionCommandPeriodMs, SinglePositiveValue) {
    EXPECT_DOUBLE_EQ(action_command_period_ms({25.0}, 1.0), 40.0);
}

TEST(ActionCommandPeriodMs, UsesMinimumOfMultipleValues) {
    // The slowest camera in the group must set the pace — triggering faster
    // than it can sustain would just reproduce the same GigE packet-loss
    // failure mode already seen from over-requested free-run fps, just
    // self-inflicted by our own trigger cadence this time.
    EXPECT_DOUBLE_EQ(action_command_period_ms({25.0, 20.0, 30.0}, 1.0), 50.0); // 1000/20
}

TEST(ActionCommandPeriodMs, IgnoresNonPositiveValues) {
    EXPECT_DOUBLE_EQ(action_command_period_ms({25.0, 0.0, -5.0, 20.0}, 1.0), 50.0); // 1000/20
}

TEST(ActionCommandPeriodMs, EmptyInputFallsBackToDefault) {
    EXPECT_DOUBLE_EQ(action_command_period_ms({}, 1.0), 1000.0 / k_default_action_fps);
}

TEST(ActionCommandPeriodMs, AllNonPositiveFallsBackToDefault) {
    EXPECT_DOUBLE_EQ(action_command_period_ms({0.0, -1.0}, 1.0), 1000.0 / k_default_action_fps);
}

TEST(ActionCommandPeriodMs, DefaultMarginSlowsPeriodBelowRawCeiling) {
    // Real room-11 finding (2026-07-27): triggering a camera at exactly its
    // own reported ceiling (no margin) caused a stable ~39-40% trigger-miss
    // rate even on a confirmed-good cable — software trigger jitter means
    // some triggers land while the camera is still mid-cycle. The default
    // margin must produce a period strictly longer (slower) than the
    // no-margin period for the same input.
    const double noMargin      = action_command_period_ms({20.0}, 1.0);
    const double defaultMargin = action_command_period_ms({20.0});
    EXPECT_GT(defaultMargin, noMargin);
    EXPECT_DOUBLE_EQ(defaultMargin, 1000.0 / (20.0 * k_default_action_margin));
}

TEST(ActionCommandPeriodMs, CustomMarginIsApplied) {
    EXPECT_DOUBLE_EQ(action_command_period_ms({20.0}, 0.5), 100.0); // 1000/(20*0.5)
}

TEST(ActionCommandPeriodMs, OutOfRangeMarginFallsBackToNoMargin) {
    EXPECT_DOUBLE_EQ(action_command_period_ms({20.0}, 0.0), 50.0);
    EXPECT_DOUBLE_EQ(action_command_period_ms({20.0}, -1.0), 50.0);
    EXPECT_DOUBLE_EQ(action_command_period_ms({20.0}, 1.5), 50.0);
}

// Real room-11 finding (2026-08-05): a magnitude-based plausibility floor
// (reject readings far below what the camera's exposure ceiling implied)
// was tried first and reverted — a longer real recording found several
// genuinely stable, repeated readings sitting well below what any fixed
// margin assumed was possible (auto-exposure converged near its ceiling in
// a dim room). Gating on real elapsed acquisition time instead needs no
// per-scene-lighting-tuned constant.
TEST(IsAchievableFpsMeasurementWarmedUp, RejectsBeforeWarmupElapses) {
    EXPECT_FALSE(is_achievable_fps_measurement_warmed_up(0.0, 3.0));
    EXPECT_FALSE(is_achievable_fps_measurement_warmed_up(2.999, 3.0));
}

TEST(IsAchievableFpsMeasurementWarmedUp, AcceptsAtOrAfterWarmupElapses) {
    EXPECT_TRUE(is_achievable_fps_measurement_warmed_up(3.0, 3.0));
    EXPECT_TRUE(is_achievable_fps_measurement_warmed_up(30.0, 3.0));
}

TEST(IsAchievableFpsMeasurementWarmedUp, NegativeSecondsMeansNotGrabbingYetAtAll) {
    // e.g. open()'s own one-time diagnostic reading, taken before
    // start_grabbing() is ever called — always rejected, regardless of how
    // permissive warmupSeconds is.
    EXPECT_FALSE(is_achievable_fps_measurement_warmed_up(-1.0, 0.0));
    EXPECT_FALSE(is_achievable_fps_measurement_warmed_up(-0.001, 3.0));
}

TEST(IsAchievableFpsMeasurementWarmedUp, NonPositiveWarmupSecondsSkipsTheWait) {
    EXPECT_TRUE(is_achievable_fps_measurement_warmed_up(0.0, 0.0));
    EXPECT_TRUE(is_achievable_fps_measurement_warmed_up(0.0, -1.0));
}

TEST(IsAchievableFpsMeasurementWarmedUp, DefaultWarmupMatchesDocumentedConstant) {
    EXPECT_FALSE(is_achievable_fps_measurement_warmed_up(k_default_fps_warmup_seconds - 0.001));
    EXPECT_TRUE(is_achievable_fps_measurement_warmed_up(k_default_fps_warmup_seconds));
}
