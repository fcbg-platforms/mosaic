#include "video/fps_readout.hpp"

namespace mosaic {

namespace {

// Microseconds per second — an exposure of E µs caps acquisition at
// 1e6 / E frames per second, since a sensor cannot produce a frame faster
// than it exposes one.
constexpr double k_us_per_second = 1'000'000.0;

bool falls_short(double fps, bool specifyFps, double configuredFps) {
    if (!specifyFps || configuredFps <= 0.0 || fps <= 0.0) {
        return false;
    }
    return fps < configuredFps * k_fps_shortfall_factor;
}

} // namespace

FpsReadout compute_fps_readout(double measuredFps, bool specifyFps, double configuredFps,
                               bool manualExposure, double exposureTimeUs) {
    // A real measurement always wins: it already accounts for exposure,
    // sensor readout and link bandwidth together, which no client-side
    // arithmetic can.
    if (measuredFps > 0.0) {
        return {FpsReadoutKind::Measured, measuredFps,
                falls_short(measuredFps, specifyFps, configuredFps)};
    }

    // Without a measurement, exposure is the only limit we can state
    // truthfully — and only when the user has actually pinned it. Under auto
    // exposure the camera picks its own value at open time, so exposureTimeUs
    // is simply not what it will use.
    if (manualExposure && exposureTimeUs > 0.0) {
        const double ceiling = k_us_per_second / exposureTimeUs;
        return {FpsReadoutKind::ExposureCeiling, ceiling,
                falls_short(ceiling, specifyFps, configuredFps)};
    }

    return {FpsReadoutKind::AwaitingMeasurement, -1.0, false};
}

} // namespace mosaic
