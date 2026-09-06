#include <gtest/gtest.h>

#include "ui/audio/time_axis.hpp"

using mosaic::time_to_x;
using mosaic::x_to_time;

namespace {
constexpr int64_t kDuration = 60'000; // one minute
constexpr double kWidth     = 800.0;
} // namespace

// ── The contract every stacked audio strip depends on ──────────────────────
//
// The waveform and the spectrogram sit one above the other showing the same
// clip. If they disagree about where a moment is, every band, tick and playhead
// on them is subtly lying, and the two still look individually plausible. These
// pin the mapping so that agreement is mechanical rather than reviewed.

TEST(TimeAxis, StartMapsToTheLeftEdgeAndEndToTheRight) {
    EXPECT_DOUBLE_EQ(time_to_x(0, kDuration, kWidth), 0.0);
    EXPECT_DOUBLE_EQ(time_to_x(kDuration, kDuration, kWidth), kWidth);
}

TEST(TimeAxis, MidpointIsCentred) {
    EXPECT_DOUBLE_EQ(time_to_x(kDuration / 2, kDuration, kWidth), kWidth / 2.0);
}

// A playhead or band edge outside the clip must land on the edge, not off the
// widget where it would either vanish or paint over a neighbour.
TEST(TimeAxis, OutOfRangeTimesClampToTheEdges) {
    EXPECT_DOUBLE_EQ(time_to_x(-5'000, kDuration, kWidth), 0.0);
    EXPECT_DOUBLE_EQ(time_to_x(kDuration * 3, kDuration, kWidth), kWidth);
}

// "Nothing loaded yet" is a real state — every one of these widgets paints
// before a clip arrives — and must not divide by zero.
TEST(TimeAxis, NonPositiveDurationYieldsZeroRatherThanDividingByZero) {
    EXPECT_DOUBLE_EQ(time_to_x(1'000, 0, kWidth), 0.0);
    EXPECT_DOUBLE_EQ(time_to_x(1'000, -1, kWidth), 0.0);
    EXPECT_EQ(x_to_time(400.0, 0, kWidth), 0);
    EXPECT_EQ(x_to_time(400.0, -1, kWidth), 0);
}

// A zero-width widget happens during construction and while a splitter is
// dragged shut.
TEST(TimeAxis, ZeroWidthIsSafeInBothDirections) {
    EXPECT_DOUBLE_EQ(time_to_x(1'000, kDuration, 0.0), 0.0);
    EXPECT_EQ(x_to_time(0.0, kDuration, 0.0), 0);
}

TEST(TimeAxis, ClickMapsBackToTime) {
    EXPECT_EQ(x_to_time(0.0, kDuration, kWidth), 0);
    EXPECT_EQ(x_to_time(kWidth / 2.0, kDuration, kWidth), kDuration / 2);
    EXPECT_EQ(x_to_time(kWidth, kDuration, kWidth), kDuration);
}

TEST(TimeAxis, ClicksOutsideTheWidgetClampIntoTheClip) {
    EXPECT_EQ(x_to_time(-100.0, kDuration, kWidth), 0);
    EXPECT_EQ(x_to_time(kWidth + 100.0, kDuration, kWidth), kDuration);
}

// Click-to-seek then redraw-the-playhead is a round trip the user performs
// constantly; it must not drift.
TEST(TimeAxis, RoundTripsWithinOnePixel) {
    for (const int64_t ms : {0LL, 1LL, 999LL, 30'000LL, 59'999LL, kDuration}) {
        const double x     = time_to_x(ms, kDuration, kWidth);
        const int64_t back = x_to_time(x, kDuration, kWidth);
        // One pixel of this clip, plus a millisecond for the truncation in
        // x_to_time.
        const int64_t tolerance = kDuration / static_cast<int64_t>(kWidth) + 1;
        EXPECT_LE(std::abs(back - ms), tolerance) << "ms = " << ms;
    }
}

// The property that actually matters between two stacked widgets: for the same
// duration, the same time yields the same x at any shared width — including
// widths where the clip's milliseconds-per-pixel is not an integer.
TEST(TimeAxis, TwoWidgetsSharingADurationAgreeAtEveryWidth) {
    for (const double w : {1.0, 37.0, 800.0, 1439.0, 3840.0}) {
        for (const int64_t ms : {0LL, 7LL, 12'345LL, kDuration}) {
            EXPECT_DOUBLE_EQ(time_to_x(ms, kDuration, w), time_to_x(ms, kDuration, w))
                << "w = " << w << " ms = " << ms;
            // And the mapping is monotonic, so ordering of events is preserved.
            if (ms > 0) {
                EXPECT_GE(time_to_x(ms, kDuration, w), time_to_x(ms - 1, kDuration, w));
            }
        }
    }
}
