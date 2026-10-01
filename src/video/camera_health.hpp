#pragma once
#include <algorithm>
#include <vector>

namespace mosaic {

// Judges, live and at a glance, whether one camera is keeping up with the
// others — so a camera delivering a fifth of its neighbours' frames is visible
// on the Live tab before a recording is started, rather than discovered in
// sync_manifest.json afterwards.
//
// Deliberately QtCore-free (standard library only) so mosaic_tests can cover
// it: that binary links only Qt6::Core and Qt6::Network, and anything decided
// inside a widget or the QML bridge is untestable. The same constraint shaped
// session_name.hpp and ui/audio/time_axis.hpp.

// ── Why fps, and why against the other cameras ─────────────────────────────
//
// Two tempting metrics are both wrong here, and it is worth writing down why
// so neither gets "fixed" back in later.
//
// *Not* a percentage of the configured frame rate. A whole rig can run below
// its configured rate together, and that is a rig-wide setting to fix (for
// months every camera here ran at ~14.4 fps against 25, held there by an
// auto-exposure limit that never reached the cameras), not a failing camera.
// Thresholding against the configured rate paints every camera amber at once,
// which is the fastest way to teach an operator to ignore the colour. A
// rig-wide shortfall is reported elsewhere: the camera cards' achievable-rate
// readout and the pre-flight check.
//
// *Not* sync_manifest.cpp's coverage_pct either, tempting as it is to match the
// number the operator reads afterwards. That is a post-hoc computation over the
// whole session: it counts master-clock ticks for which a camera has a frame
// within half a tick period, which needs every frame's timestamp and a shared
// tick grid derived from all cameras' first and last frames. It cannot be
// evaluated incrementally. An approximation would disagree with the manifest —
// the same fault reads as 21% by frame-rate ratio and 12% by coverage — and two
// percentages that disagree about one problem are worse than one honest number.
//
// So: report the measured frame rate, and judge it against what the *other*
// cameras are managing right now. That is the comparison which actually
// separates "this rig is bandwidth-limited" (everything low together, normal
// here) from "this camera has stopped receiving trigger broadcasts" (one camera
// far below its peers, the failure worth interrupting someone for).

enum class CameraHealth {
    Unknown, ///< Not grabbing, or too few peers to compare against.
    Ok,      ///< Keeping up with its peers.
    Lagging, ///< Noticeably behind them.
    Stalled, ///< Delivering almost nothing while others are fine.
};

/// Fraction of the peer median below which a camera reads as Lagging.
inline constexpr double k_camera_lagging_ratio = 0.70;
/// …and below which it reads as Stalled.
///
/// Calibrated against the failure that prompted this, not picked for looking
/// round: config indices 2 and 3 (Cameras 3 and 4 on screen) delivered 5.3 and
/// 2.3 fps while their peers managed 14.4, i.e. ratios of 0.37 and 0.16. A
/// first attempt at 0.30 put the 5.3 fps
/// camera — which captured 12 frames where the others captured 44 — in amber
/// rather than red, and the test below caught it. Anything under 0.40 of what
/// the rest of the rig is managing is not "behind", it is not working.
inline constexpr double k_camera_stalled_ratio = 0.40;

/// A running camera whose newest frame is older than this is delivering
/// nothing, whatever its last measured rate says.
inline constexpr double k_camera_stale_frame_sec = 2.0;

/// The rate to judge a camera by: its measured rate while frames are still
/// arriving, 0 once they have stopped.
///
/// Needed because the grabber's rate (VideoGrabber::current_fps()) is only
/// recomputed when a good frame arrives. A camera whose link drops — every
/// RetrieveResult timing out, or every frame arriving corrupted — goes on
/// reporting the healthy rate it had before, indefinitely, and would read Ok
/// against its peers at exactly the moment it matters most. The age of its
/// newest frame does not have that blind spot.
///
/// @param measuredFps      CameraStats::fps.
/// @param lastFrameAgeSec  Seconds since the newest frame; < 0 when none has
///                         arrived yet (also 0 fps — a camera that has never
///                         delivered while its peers do is not delivering).
[[nodiscard]] inline double delivered_fps(double measuredFps, double lastFrameAgeSec) {
    if (lastFrameAgeSec < 0.0 || lastFrameAgeSec > k_camera_stale_frame_sec) {
        return 0.0;
    }
    return measuredFps > 0.0 ? measuredFps : 0.0;
}

/// Median of the frame rates of cameras that are actually grabbing.
///
/// Median, not mean: with five cameras, two of them collapsed drags a mean far
/// enough down that the collapsed pair start to look merely below-average. The
/// median stays pinned to what a healthy camera on this rig is doing, which is
/// the number the comparison is supposed to be against.
///
/// Returns 0.0 when nothing is running, which classify_camera_health() reads as
/// "no basis for comparison".
[[nodiscard]] inline double peer_median_fps(std::vector<double> runningFps) {
    if (runningFps.empty()) {
        return 0.0;
    }
    const std::size_t mid = runningFps.size() / 2;
    std::nth_element(runningFps.begin(), runningFps.begin() + static_cast<long>(mid),
                     runningFps.end());
    const double upper = runningFps[mid];
    if (runningFps.size() % 2 != 0) {
        return upper;
    }
    // Even count: average the two middle values. max_element over the lower
    // half is exact after nth_element, which only guarantees a partition.
    const double lower =
        *std::max_element(runningFps.begin(), runningFps.begin() + static_cast<long>(mid));
    return (lower + upper) / 2.0;
}

/// Where one camera sits relative to its peers.
///
/// @param fps         This camera's measured rate.
/// @param medianFps   What the *other* running cameras are managing. Use
///                    classify_cameras() rather than computing this yourself;
///                    which cameras count as "other" is the subtle part.
/// @param running     VideoManager::CameraStats::grabberRunning. A camera that
///                    is not grabbing is Unknown, not Stalled: it is not
///                    failing, it is switched off, and colouring it red would
///                    report a fault that does not exist.
[[nodiscard]] inline CameraHealth classify_camera_health(double fps, double medianFps,
                                                         bool running) {
    if (!running) {
        return CameraHealth::Unknown;
    }
    // No healthy baseline to measure against — a single camera has no peers,
    // and a rig where everything has stopped has nothing to compare. Saying
    // "Unknown" is the honest answer; inventing a threshold from the configured
    // rate here would resurrect exactly the false-alarm problem described above.
    if (!(medianFps > 0.0)) { // also catches NaN
        return CameraHealth::Unknown;
    }
    const double ratio = fps / medianFps;
    if (ratio < k_camera_stalled_ratio) {
        return CameraHealth::Stalled;
    }
    if (ratio < k_camera_lagging_ratio) {
        return CameraHealth::Lagging;
    }
    return CameraHealth::Ok;
}

/// Classify every camera at once, each against its peers.
///
/// The reason this exists rather than one shared median: a camera must be
/// judged against the cameras *other than itself*. Including it in its own
/// baseline sounds harmless and is not. Four cameras at 14.4, 14.4, 5.3 and
/// 2.3 have a combined median of 9.85 — dragged halfway to the failures by the
/// failures — and 5.3/9.85 = 0.54 reads as merely Lagging, when 5.3 fps beside
/// peers doing 14.4 is the exact case k_camera_stalled_ratio was calibrated to
/// catch. Excluding self, that camera's peers are 14.4, 14.4 and 2.3, median
/// 14.4, ratio 0.37, correctly Stalled.
///
/// The five-camera shape this was first written against happens to hide the
/// problem: with an odd count the median stays pinned on a healthy value. Even
/// counts are where it bites, which is why it went unnoticed.
///
/// @param fps      Measured rate per camera, indexed by *configured* camera
///                 index — the same index QML uses for tiles and chips.
/// @param running  Parallel to `fps`; a camera that is not grabbing is left out
///                 of everyone's baseline and reports Unknown itself.
[[nodiscard]] inline std::vector<CameraHealth> classify_cameras(const std::vector<double>& fps,
                                                                const std::vector<bool>& running) {
    std::vector<CameraHealth> out(fps.size(), CameraHealth::Unknown);

    for (std::size_t self = 0; self < fps.size(); ++self) {
        const bool selfRunning = self < running.size() && running[self];
        if (!selfRunning) {
            continue; // stays Unknown — switched off is not broken
        }
        std::vector<double> peers;
        peers.reserve(fps.size());
        for (std::size_t i = 0; i < fps.size(); ++i) {
            if (i != self && i < running.size() && running[i]) {
                peers.push_back(fps[i]);
            }
        }
        // No peers at all: a lone camera has no second opinion available, so
        // peer_median_fps() returns 0 and this reports Unknown. Correct — there
        // is genuinely nothing to compare it against.
        out[self] =
            classify_camera_health(fps[self], peer_median_fps(std::move(peers)), selfRunning);
    }
    return out;
}

} // namespace mosaic
