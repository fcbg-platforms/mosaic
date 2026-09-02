#pragma once

namespace mosaic {

// Decides what a camera's "achievable frame rate" readout should say, given
// the camera's own measured rate plus the exposure/frame-rate settings the
// user currently has selected.
//
// Pure, Qt-free and hardware-free so it can be unit-tested directly — the
// same split already used for rms_quality_for()/pose_tracking_quality_for():
// this decides *what is true*, and CameraCardW decides how to word and colour
// it.
//
// The one thing this deliberately never does is predict a frame rate from the
// exposure time and present it as fact. Exposure is only one of three limits
// (sensor readout time and GigE bandwidth are the others), and on this rig
// bandwidth is the binding constraint on at least one camera — so a
// client-side 1/exposure figure would be confidently wrong exactly when it
// matters most. The camera's own ResultingFrameRate already accounts for all
// three, so a real measurement is always preferred; the exposure figure is
// only ever offered as an upper *bound*, clearly distinguished from a
// measurement (see FpsReadoutKind::ExposureCeiling).

// A camera counts as unable to sustain its configured rate below this
// fraction of it. Shared with VideoGrabber::refresh_achievable_fps()'s own
// "requested X fps but the camera can only sustain ~Y" log warning, so the
// on-screen readout and the log can never disagree about what counts as a
// shortfall.
inline constexpr double k_fps_shortfall_factor = 0.9;

// Smallest change in a camera's measured rate worth telling the UI about.
// refresh_achievable_fps() re-measures every ~2s per camera and a stable
// camera's reading jitters in the third decimal place, so announcing every
// re-read would repaint every card's readout continuously to no purpose.
inline constexpr double k_fps_change_epsilon = 0.05;

enum class FpsReadoutKind {
    // No trustworthy measurement yet: the camera isn't open, or it opened so
    // recently that its own reading is still inside the warm-up window (see
    // is_achievable_fps_measurement_warmed_up()). Exposure is on auto, so
    // there's no honest upper bound to offer either — the camera picks the
    // exposure itself and we can't know it until it runs.
    AwaitingMeasurement,
    // No measurement, but exposure is manual, so the exposure time alone
    // imposes a hard ceiling: a sensor cannot produce frames faster than it
    // exposes them. An upper bound, never a prediction — the real rate is
    // usually lower once readout and bandwidth are accounted for.
    ExposureCeiling,
    // The camera's own measured ResultingFrameRate.
    Measured,
};

struct FpsReadout {
    FpsReadoutKind kind = FpsReadoutKind::AwaitingMeasurement;
    // Frames per second for Measured/ExposureCeiling; -1.0 for
    // AwaitingMeasurement.
    double fps = -1.0;
    // True when `fps` falls meaningfully short of the configured frame rate.
    // Always false when the user hasn't pinned a frame rate (specifyFps), and
    // always false for AwaitingMeasurement — there is nothing to compare.
    bool belowConfigured = false;
};

// @param measuredFps    VideoGrabber::achievable_fps() — <= 0 means "no
//                       trustworthy reading yet", which is its documented
//                       sentinel, not an error.
// @param specifyFps     CameraParameters::specifyFps — whether the user has
//                       pinned a target frame rate at all.
// @param configuredFps  CameraParameters::fps.
// @param manualExposure True iff CameraParameters::exposureAuto == "Off".
//                       Under "Once"/"Continuous" the camera chooses its own
//                       exposure, so exposureTimeUs says nothing about what
//                       it will actually use.
// @param exposureTimeUs CameraParameters::exposureTimeUs.
[[nodiscard]] FpsReadout compute_fps_readout(double measuredFps, bool specifyFps,
                                             double configuredFps, bool manualExposure,
                                             double exposureTimeUs);

} // namespace mosaic
