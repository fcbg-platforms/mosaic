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

// Whether a measured rate sits close enough to the configured one — on
// *either* side — to call the configured rate the binding constraint. Note
// this can never be true at the same time as falls_short(): the band starts
// at 99% of configured, the shortfall ends below 90% of it.
//
// The upper half of the band matters as much as the lower. CameraCardW's
// frame-rate spinbox relabels synchronously on every valueChanged while the
// last measurement is still whatever the camera reported for the *previous*
// target (it only catches up after the live re-apply, up to ~2 s later), so
// dragging 30 fps down to 15 would otherwise render "30.0 fps — capped by the
// 15.0 fps setting": a cap the number on screen visibly doubles.
bool is_at_configured_cap(double fps, bool specifyFps, double configuredFps) {
    if (!specifyFps || configuredFps <= 0.0 || fps <= 0.0) {
        return false;
    }
    return fps >= configuredFps * (1.0 - k_fps_at_cap_tolerance) &&
           fps <= configuredFps * (1.0 + k_fps_at_cap_tolerance);
}

} // namespace

FpsReadout compute_fps_readout(double measuredFps, bool specifyFps, double configuredFps,
                               bool manualExposure, double exposureTimeUs) {
    // A real measurement always wins: it already accounts for exposure,
    // sensor readout and link bandwidth together, which no client-side
    // arithmetic can.
    if (measuredFps > 0.0) {
        FpsReadout out{FpsReadoutKind::Measured, measuredFps,
                       falls_short(measuredFps, specifyFps, configuredFps)};
        // Only classifiable against a rate the user actually pinned: a
        // free-running camera reports whatever it can manage, and there is no
        // "cap" for that to be at.
        if (!specifyFps || configuredFps <= 0.0) {
            return out;
        }
        if (is_at_configured_cap(measuredFps, specifyFps, configuredFps)) {
            out.limitedBy           = FpsLimit::ConfiguredRate;
            out.exposureCrossoverUs = k_us_per_second / configuredFps;
        } else if (measuredFps < configuredFps) {
            out.limitedBy = FpsLimit::Other;
        }
        // Faster than the rate it was pinned to: AcquisitionFrameRate isn't
        // being honoured, so neither classification applies — the cap plainly
        // isn't binding, and calling it Other would contradict that enumerator's
        // own "measurably below the configured rate" meaning. Stays Unknown.
        return out;
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
