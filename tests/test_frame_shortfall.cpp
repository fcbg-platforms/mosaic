#include <gtest/gtest.h>

#include "video/frame_shortfall.hpp"

using mosaic::classify_frame_shortfall;
using mosaic::FrameShortfall;

// ── classify_frame_shortfall ───────────────────────────────────────────────
//
// The two faults look identical from the outside — "this camera delivered
// fewer frames than the others" — and need opposite remedies. The old
// diagnostic asserted MissedTriggers unconditionally and never read the
// packet-loss counter sitting on the same object.

// 2026-09-07, "[Camera 3]" in the log (config index 3, on screen Camera 4),
// real numbers: 74 ticks, 21 captured, 308 corrupted.
// The cameras fired and the images were destroyed in transit.
TEST(FrameShortfall, TheObservedFailureIsPacketLossNotMissedTriggers) {
    EXPECT_EQ(classify_frame_shortfall(74, 21, 308), FrameShortfall::PacketLoss);
    EXPECT_EQ(classify_frame_shortfall(74, 25, 212), FrameShortfall::PacketLoss); // config index 2
}

// The 2026-07-27 shape, from room11-camera-setup: a camera missing ~34% of its
// triggers with *zero* corrupted frames. That one really was the trigger path —
// the zero-margin pacing bug — and must still classify that way, or fixing this
// message would just move the misdiagnosis to the other side.
TEST(FrameShortfall, AZeroCorruptionShortfallIsStillMissedTriggers) {
    EXPECT_EQ(classify_frame_shortfall(74, 49, 0), FrameShortfall::MissedTriggers);
    EXPECT_EQ(classify_frame_shortfall(103, 70, 0), FrameShortfall::MissedTriggers);
}

TEST(FrameShortfall, ASmallGapIsNotWorthNaming) {
    EXPECT_EQ(classify_frame_shortfall(100, 100, 0), FrameShortfall::None);
    EXPECT_EQ(classify_frame_shortfall(100, 96, 0), FrameShortfall::None);
    EXPECT_EQ(classify_frame_shortfall(100, 96, 50), FrameShortfall::None); // slack wins first
}

// A camera can suffer both at once, and picking one would send the operator
// after half the problem.
TEST(FrameShortfall, BothIsReportedWhenBothAreMaterial) {
    // 100 missing, 50 corrupted: half explained by transport, half not.
    EXPECT_EQ(classify_frame_shortfall(200, 100, 50), FrameShortfall::Both);
}

// The counters are read from different threads a moment apart, so a camera
// losing nearly everything can report more corrupted frames than the gap. That
// must read as "all of it", not produce a share above 1 or flip the verdict.
TEST(FrameShortfall, MoreCorruptedFramesThanTheGapIsClampedNotConfused) {
    EXPECT_EQ(classify_frame_shortfall(74, 21, 100000), FrameShortfall::PacketLoss);
    EXPECT_EQ(classify_frame_shortfall(74, 21, 53), FrameShortfall::PacketLoss);
}

// Nothing fired yet, or the camera is ahead: no verdict, no division by zero.
TEST(FrameShortfall, DegenerateCountsAreNone) {
    EXPECT_EQ(classify_frame_shortfall(0, 0, 0), FrameShortfall::None);
    EXPECT_EQ(classify_frame_shortfall(50, 60, 0), FrameShortfall::None); // captured > fired
    EXPECT_EQ(classify_frame_shortfall(0, 0, 999), FrameShortfall::None);
}

// The threshold sits where the constant says, and on the packet-loss side of
// halfway on purpose: a corrupted frame is evidence, a missed trigger is only
// ever inferred from an absence.
TEST(FrameShortfall, TheThresholdSitsWhereTheConstantSays) {
    // 100 missing. 25 corrupted = exactly k_packet_loss_share.
    EXPECT_EQ(classify_frame_shortfall(200, 100, 25), FrameShortfall::Both);
    EXPECT_EQ(classify_frame_shortfall(200, 100, 24), FrameShortfall::MissedTriggers);
    // All of the gap corrupted, nothing left to blame on triggers.
    EXPECT_EQ(classify_frame_shortfall(200, 100, 100), FrameShortfall::PacketLoss);
    // Enough corrupted to matter, too few triggers missing to be worth naming.
    EXPECT_EQ(classify_frame_shortfall(200, 100, 97), FrameShortfall::PacketLoss);
}
