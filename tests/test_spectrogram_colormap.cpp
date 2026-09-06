#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "ui/audio/spectrogram_colormap.hpp"

using mosaic::Rgb8;
using mosaic::spectrogram_color_at;
using mosaic::spectrogram_color_table;
using mosaic::SpectrogramColormap;

namespace {

// Rec. 709 relative luminance — the quantity a perceptually uniform colormap
// is supposed to ramp monotonically. Everything below hangs off this.
double luma(Rgb8 c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }

} // namespace

TEST(SpectrogramColormap, TableCoversEveryByteValue) {
    EXPECT_EQ(spectrogram_color_table(SpectrogramColormap::Magma).size(), 256u);
    EXPECT_EQ(spectrogram_color_table(SpectrogramColormap::Grayscale).size(), 256u);
}

// The reason magma was chosen at all. A single mistyped anchor breaks this
// immediately, whereas by eye the spectrogram would still look like a
// spectrogram — it would just misrepresent which parts are loud.
//
// The one-unit tolerance is a rounding allowance, not slack. Interpolated
// channels are rounded to 8 bits independently, so a step can round green down
// while rounding red and blue up and lose a fraction of a luminance unit even
// though the underlying ramp rises. Measured across this table: exactly one
// such step, entry 45 -> 46, at -0.43 units — a quarter of the quantisation
// step and far below anything visible. A genuinely wrong anchor moves
// luminance by tens of units, so this still catches what it is here to catch.
TEST(SpectrogramColormap, MagmaLuminanceNeverMeaningfullyDecreases) {
    const auto table = spectrogram_color_table(SpectrogramColormap::Magma);
    for (std::size_t i = 1; i < table.size(); ++i) {
        EXPECT_GE(luma(table[i]), luma(table[i - 1]) - 1.0)
            << "luminance dropped between entries " << i - 1 << " and " << i;
    }
}

// Monotonic alone would be satisfied by a flat ramp. Require real progress
// across every window, which catches a duplicated or transposed anchor run
// that a global endpoint check would sail past.
TEST(SpectrogramColormap, MagmaKeepsClimbingAcrossEveryWindow) {
    const auto table = spectrogram_color_table(SpectrogramColormap::Magma);
    for (std::size_t i = 0; i + 16 < table.size(); i += 16) {
        EXPECT_GT(luma(table[i + 16]), luma(table[i]) + 1.0)
            << "no meaningful luminance change across entries " << i << "-" << i + 16;
    }
}

// The endpoints are what make this map usable on this particular UI: the
// quietest entry has to disappear into the app's #080816 background (luma ~9),
// or every silent stretch of a recording reads as signal.
TEST(SpectrogramColormap, MagmaEndpointsSuitTheDarkTheme) {
    const auto table = spectrogram_color_table(SpectrogramColormap::Magma);
    EXPECT_LT(luma(table.front()), 12.0);
    EXPECT_GT(luma(table.back()), 230.0);
}

TEST(SpectrogramColormap, GrayscaleIsTheIdentityRamp) {
    const auto table = spectrogram_color_table(SpectrogramColormap::Grayscale);
    for (std::size_t i = 0; i < table.size(); ++i) {
        const auto v = static_cast<std::uint8_t>(i);
        EXPECT_EQ(table[i], (Rgb8{v, v, v})) << "entry " << i;
    }
}

// The widget builds its lookup table from one of these and samples with the
// other; if they ever disagreed, the rendered image would not match anything
// that could be tested.
TEST(SpectrogramColormap, PointSamplingMatchesTheTableExactly) {
    for (const auto map : {SpectrogramColormap::Magma, SpectrogramColormap::Grayscale}) {
        const auto table = spectrogram_color_table(map);
        for (std::size_t i = 0; i < table.size(); ++i) {
            EXPECT_EQ(spectrogram_color_at(map, static_cast<double>(i) / 255.0), table[i])
                << "entry " << i;
        }
    }
}

// dB normalisation upstream can hand this a value outside [0,1] — or a NaN,
// from an all-silent frame. Neither may index out of range.
TEST(SpectrogramColormap, OutOfRangeAndNaNClampInsteadOfIndexingOutOfBounds) {
    for (const auto map : {SpectrogramColormap::Magma, SpectrogramColormap::Grayscale}) {
        const auto table = spectrogram_color_table(map);
        EXPECT_EQ(spectrogram_color_at(map, -5.0), table.front());
        EXPECT_EQ(spectrogram_color_at(map, 0.0), table.front());
        EXPECT_EQ(spectrogram_color_at(map, 1.0), table.back());
        EXPECT_EQ(spectrogram_color_at(map, 5.0), table.back());
        // NaN reads as "nothing here", the safe direction.
        EXPECT_EQ(spectrogram_color_at(map, std::numeric_limits<double>::quiet_NaN()),
                  table.front());
    }
}
