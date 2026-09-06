#pragma once
#include <array>
#include <cstdint>

namespace mosaic {

// Maps a spectrogram's 8-bit intensity onto a colour.
//
// Deliberately free of every Qt type, including QRgb — that lives in a QtGui
// header, and this file's whole reason for being separate is that it compiles
// into mosaic_tests, which links Qt6::Core and Qt6::Network only. The widget
// converts to QRgb once, when it builds its lookup table.
//
// A wrong colormap is the kind of defect that never announces itself: the
// spectrogram still looks like a spectrogram, it just misrepresents which
// parts are loud. Hence a table that can be asserted on (see
// tests/test_spectrogram_colormap.cpp) rather than eyeballed.

struct Rgb8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    friend constexpr bool operator==(Rgb8, Rgb8) = default;
};

enum class SpectrogramColormap {
    /// Perceptually uniform, near-black at the low end.
    ///
    /// Chosen over viridis for one specific reason: this app's background is
    /// #080816, and viridis' low end is #440154 — a saturated purple that
    /// reads as "there is signal here" across every silent stretch of a
    /// recording. Magma's #000004 dissolves into the background instead, so
    /// silence looks like silence. Its cooler mid-tones also collide less with
    /// the #ffdd55 playhead and the pitch line drawn over the top.
    Magma,
    /// Straight luminance ramp. Kept partly as an escape hatch and partly
    /// because being analytically defined makes "was the table applied at
    /// all?" a trivial thing to test.
    Grayscale,
};

/// The full 256-entry table, indexed by the 8-bit intensity read from the
/// spectrogram image (0 = quietest).
[[nodiscard]] std::array<Rgb8, 256> spectrogram_color_table(SpectrogramColormap map);

/// Single-sample evaluation for a `t` in [0, 1]. Values outside are clamped;
/// NaN yields entry 0 rather than an out-of-bounds index.
///
/// Guaranteed consistent with the table: spectrogram_color_at(m, i / 255.0)
/// equals spectrogram_color_table(m)[i] for every i.
[[nodiscard]] Rgb8 spectrogram_color_at(SpectrogramColormap map, double t);

} // namespace mosaic
