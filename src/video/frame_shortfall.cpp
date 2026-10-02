#include "video/frame_shortfall.hpp"

#include <algorithm>

namespace mosaic {

FrameShortfall classify_frame_shortfall(int64_t ticksFired, int64_t framesCaptured,
                                        int64_t incompleteFrames) {
    const int64_t missing = ticksFired - framesCaptured;
    if (missing < k_min_reportable_shortfall) {
        return FrameShortfall::None;
    }

    // Corrupted frames are counted against the gap they actually explain.
    // Clamped because the counters are read from different threads a moment
    // apart and cover slightly different windows: a camera losing almost
    // everything can report more corrupted frames than the gap, and that must
    // read as "all of it", not as a share above 1.
    const int64_t corrupted = std::clamp<int64_t>(incompleteFrames, 0, missing);
    const double share      = static_cast<double>(corrupted) / static_cast<double>(missing);

    const bool lossMaterial    = share >= k_packet_loss_share;
    const bool triggersMissing = (missing - corrupted) >= k_min_reportable_shortfall;

    if (lossMaterial && triggersMissing) {
        return FrameShortfall::Both;
    }
    if (lossMaterial) {
        return FrameShortfall::PacketLoss;
    }
    return FrameShortfall::MissedTriggers;
}

} // namespace mosaic
