#include "ui/audio/spectrogram_colormap.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic {

namespace {

// 32 evenly-spaced samples of matplotlib's "magma", linearly interpolated to
// fill 256 entries. Magma is smooth enough that the interpolated table is
// visually indistinguishable from the full 256-entry original, and 32 rows stay
// readable and reviewable in a way that 256 would not.
//
// Provenance: matplotlib's _cm_listed.py, released into the public domain
// (CC0) by Nathaniel Smith and Stéfan van der Walt. No attribution is required;
// it is recorded because a table of magic numbers with no origin is impossible
// to check. Regenerate with:
//     python -c "from matplotlib import colormaps; cm=colormaps['magma']; \
//                print([tuple(round(c*255) for c in cm(i/31)[:3]) for i in range(32)])"
//
// Interpolating linearly in sRGB is not colorimetrically pure, but between
// stops this closely spaced the error is far below a just-noticeable
// difference, and it keeps the luminance ramp monotonic — which is the
// property the tests actually pin, and the one a reader of the spectrogram
// depends on.
constexpr std::array<Rgb8, 32> k_magma_stops{{
    {0, 0, 4},       {3, 3, 18},      {10, 8, 34},     {19, 13, 52},    {30, 17, 73},
    {42, 17, 92},    {56, 16, 108},   {69, 16, 119},   {84, 19, 125},   {96, 24, 128},
    {109, 29, 129},  {121, 34, 130},  {136, 39, 129},  {148, 44, 128},  {161, 48, 126},
    {174, 52, 123},  {189, 57, 119},  {202, 62, 114},  {214, 69, 108},  {226, 77, 102},
    {236, 88, 96},   {243, 101, 92},  {248, 116, 92},  {251, 131, 95},  {253, 148, 103},
    {254, 163, 111}, {254, 178, 122}, {254, 193, 133}, {254, 209, 148}, {253, 224, 161},
    {252, 238, 176}, {252, 253, 191},
}};

std::uint8_t lerp_channel(std::uint8_t a, std::uint8_t b, double f) {
    const double v = static_cast<double>(a) + (static_cast<double>(b) - a) * f;
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
}

Rgb8 magma_at(double t) {
    // t is already clamped to [0, 1] by the callers.
    const double scaled = t * (k_magma_stops.size() - 1);
    const auto lo       = static_cast<std::size_t>(scaled);
    if (lo >= k_magma_stops.size() - 1) {
        return k_magma_stops.back();
    }
    const Rgb8 a   = k_magma_stops[lo];
    const Rgb8 b   = k_magma_stops[lo + 1];
    const double f = scaled - static_cast<double>(lo);
    return {lerp_channel(a.r, b.r, f), lerp_channel(a.g, b.g, f), lerp_channel(a.b, b.b, f)};
}

} // namespace

std::array<Rgb8, 256> spectrogram_color_table(SpectrogramColormap map) {
    std::array<Rgb8, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i] = spectrogram_color_at(map, static_cast<double>(i) / 255.0);
    }
    return table;
}

Rgb8 spectrogram_color_at(SpectrogramColormap map, double t) {
    // NaN fails every comparison, so clamp() would propagate it into the index
    // arithmetic. Send it to the quietest entry instead: an unrenderable value
    // should read as "nothing here", never crash and never index out of range.
    if (std::isnan(t)) {
        t = 0.0;
    }
    t = std::clamp(t, 0.0, 1.0);

    if (map == SpectrogramColormap::Grayscale) {
        const auto v = static_cast<std::uint8_t>(std::lround(t * 255.0));
        return {v, v, v};
    }
    return magma_at(t);
}

} // namespace mosaic
