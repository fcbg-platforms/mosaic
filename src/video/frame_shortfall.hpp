#pragma once
#include <cstdint>

namespace mosaic {

// Why a hardware-triggered camera is short of frames.
//
// Two very different faults produce the same symptom — "this camera delivered
// fewer frames than the triggers it was sent" — and need opposite remedies.
// The diagnostic in VideoManager's ActionCommandTicker used to assert missed
// triggers every time, without reading the corrupted-frame counter sitting on
// the same grabber; a camera whose images were being destroyed in transit
// reported a trigger problem and sent whoever read it after the wrong thing.
//
// Standard library only, so mosaic_tests covers it.

enum class FrameShortfall {
    None,           ///< Delivering as expected.
    MissedTriggers, ///< Triggers never arrived, or landed mid-readout. Check the trigger path.
    PacketLoss,     ///< Frames were sent and corrupted in transit. Check bandwidth and cabling.
    Both,           ///< Materially both — worth naming both rather than picking.
};

/// Fraction of a shortfall that has to be explained by corrupted frames before
/// it counts as a packet-loss problem. Well below half on purpose: a corrupted
/// frame is direct evidence of a transport fault, whereas a "missed trigger" is
/// only ever inferred from the absence of something.
inline constexpr double k_packet_loss_share = 0.25;

/// A shortfall smaller than this many frames is not worth naming: the
/// readiness barrier lets a camera join a few ticks late, and normal jitter
/// costs a frame here and there.
inline constexpr int64_t k_min_reportable_shortfall = 5;

/// Classifies a shortfall from counters VideoManager already has.
///
/// @param ticksFired        Action commands issued to this camera.
/// @param framesCaptured    Complete frames that arrived.
/// @param incompleteFrames  Frames that started arriving and were corrupted
///                          (VideoGrabber::incomplete_frames_total()). These
///                          never reach framesCaptured, so they are exactly the
///                          part of the gap that is *not* a missed trigger.
///
/// All three must cover the same window — the caller baselines the cumulative
/// incomplete counter to the moment ticking started.
[[nodiscard]] FrameShortfall classify_frame_shortfall(int64_t ticksFired, int64_t framesCaptured,
                                                      int64_t incompleteFrames);

} // namespace mosaic
