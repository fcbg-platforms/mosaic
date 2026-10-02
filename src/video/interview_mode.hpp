#pragma once

namespace mosaic {

// Arithmetic behind the interview-mode settings panel: where a centred crop
// starts, how much of the camera's link a crop and rate would use, and the
// longest exposure a rate allows.
//
// Pure and Qt-free so mosaic_tests covers it; the panel only words the
// results. See InterviewSettings (core/settings.hpp) for the mode itself.

/// Bytes per pixel these cameras actually send. They are configured for BGR8
/// but reject it, and every fallback but YUV422Packed — confirmed on the room
/// 11 hardware — so the wire carries 2 bytes per pixel, not 1 (Bayer) or 3.
inline constexpr double k_interview_wire_bytes_per_pixel = 2.0;

/// Raw payload rate of a gigabit link, in MB/s (1e6 bytes). The usable rate
/// is a few percent lower once GigE Vision packet headers are paid for, which
/// is why the panel shows the load as a share of this rather than promising
/// that anything under it will work.
inline constexpr double k_gige_link_mb_per_s = 125.0;

/// The offset that centres a span of `size` pixels in a frame of `frame`
/// pixels, rounded down to a multiple of 4 — offsets on this camera
/// generation move in steps of 2 or 4, and the grabber would round anyway,
/// but showing the operator the value that will actually be applied is
/// better than showing one it will quietly change. 0 when the crop is as
/// large as the frame or larger.
[[nodiscard]] constexpr int centred_offset(int frame, int size) {
    if (size >= frame) {
        return 0;
    }
    return ((frame - size) / 2) / 4 * 4;
}

/// The video payload a crop and rate put on the camera's link, in MB/s.
[[nodiscard]] constexpr double stream_mb_per_s(
    int width, int height, double fps, double bytesPerPixel = k_interview_wire_bytes_per_pixel) {
    if (width <= 0 || height <= 0 || !(fps > 0.0)) {
        return 0.0;
    }
    return static_cast<double>(width) * height * bytesPerPixel * fps / 1e6;
}

/// The longest exposure, in µs, that still allows `fps`: a sensor cannot
/// deliver frames faster than it exposes them (see fps_readout.hpp). -1 for a
/// non-positive rate.
[[nodiscard]] constexpr double max_exposure_us_for_fps(double fps) {
    return fps > 0.0 ? 1e6 / fps : -1.0;
}

/// Whether a crop lies inside a frame of the given size.
[[nodiscard]] constexpr bool crop_fits(int frameWidth, int frameHeight, int width, int height,
                                       int offsetX, int offsetY) {
    return width > 0 && height > 0 && offsetX >= 0 && offsetY >= 0 &&
           offsetX + width <= frameWidth && offsetY + height <= frameHeight;
}

} // namespace mosaic
