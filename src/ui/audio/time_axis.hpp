#pragma once
#include <algorithm>
#include <cstdint>

namespace mosaic {

// Maps a clip-relative time onto a horizontal pixel, and back.
//
// Trivial arithmetic, deliberately given a name and a home. Several widgets
// draw the same audio clip stacked one above another — the waveform's speaker
// bands, its time ticks, its playhead and its click-to-seek, plus the
// spectrogram below it — and every one of them has to agree, to the pixel,
// about where a given moment sits. When each open-coded the same expression
// that agreement was a property a reviewer had to re-derive four times; a
// shared function makes it something the compiler enforces instead.
//
// It also makes the agreement *testable*. Everything that consumes these lives
// in a QWidget, and mosaic_tests links only Qt6::Core/Network, so a widget's
// own arithmetic cannot be reached from a test at all. Pulling it out here —
// QtCore-only, in fact standard-library-only — is the difference between the
// alignment being covered and being hoped for.
//
// The convention these encode: **edge to edge, no gutter**. x == 0 is the
// start of the clip and x == widthPx is its end, with nothing reserved at
// either side. Any widget that wants axis labels must overlay them on the plot
// rather than inset it, because a gutter on one strip and not its neighbour
// shifts the time origin between them, and the two then disagree about where a
// moment is while both looking perfectly reasonable.

/// Clip-relative milliseconds to a horizontal pixel offset.
///
/// Clamped to [0, widthPx]: callers draw a playhead or a band edge with these,
/// and a value outside the widget is never what is wanted. Returns 0 for a
/// non-positive duration — the "nothing loaded yet" state, which must not
/// divide by zero.
[[nodiscard]] inline double time_to_x(int64_t ms, int64_t durationMs, double widthPx) {
    if (durationMs <= 0) {
        return 0.0;
    }
    const double frac = static_cast<double>(ms) / static_cast<double>(durationMs);
    return std::clamp(frac, 0.0, 1.0) * widthPx;
}

/// The inverse: a horizontal pixel offset back to clip-relative milliseconds.
/// Used for click-to-seek. Same clamping and same zero-duration guard.
[[nodiscard]] inline int64_t x_to_time(double x, int64_t durationMs, double widthPx) {
    if (durationMs <= 0 || widthPx <= 0.0) {
        return 0;
    }
    const double frac = std::clamp(x / widthPx, 0.0, 1.0);
    return static_cast<int64_t>(frac * static_cast<double>(durationMs));
}

} // namespace mosaic
