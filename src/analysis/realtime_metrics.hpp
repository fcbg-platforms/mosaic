#pragma once
#include "calibration/rms_quality.hpp"
#include <QVector>
#include <cstdint>
#include <deque>
#include <optional>

namespace mosaic {

/// @brief Buckets a stream of detected/not-detected observations into fixed
/// time windows for a live sparkline — the "avoid a wall of tiny per-frame
/// dots" aggregation the Real-time tab's tile footer uses instead of raw
/// per-frame data.
///
/// Buckets roll forward as push() is called with newer timestamps; once
/// bucketCount buckets exist, the oldest is evicted. A bucket with zero
/// observations is never fabricated — bucket_rates() only ever reports
/// buckets that actually received at least one push().
class DetectionRateTracker {
public:
    explicit DetectionRateTracker(int bucketCount = 24, int64_t bucketDurationMs = 5000);

    /// Records one detection observation (true = a subject/keypoint set was
    /// present this frame) at the given monotonic timestamp.
    void push(bool detected, int64_t timestampMs);

    /// Overall detection rate across every currently-held bucket, in [0,1].
    /// Empty if push() has never been called.
    [[nodiscard]] std::optional<double> rate() const;

    /// Per-bucket rates, oldest first, for sparkline rendering.
    [[nodiscard]] QVector<double> bucket_rates() const;

    [[nodiscard]] int bucket_count() const { return static_cast<int>(buckets_.size()); }

private:
    struct Bucket {
        int64_t index = 0;   // timestampMs / bucketDurationMs_
        int     hits  = 0;
        int     total = 0;
    };

    int             maxBuckets_;
    int64_t         bucketDurationMs_;
    std::deque<Bucket> buckets_;
};

/// @brief Buckets a live pose-detection rate (fraction of recent frames with
/// at least one detected subject, [0,1]) into the same green/amber/red status
/// vocabulary already established for calibration quality
/// (src/calibration/rms_quality.hpp) — high rate is good, unlike RMS error
/// where low is good, so the thresholds run in the opposite direction.
[[nodiscard]] RmsQuality pose_tracking_quality_for(double detectionRate);

/// Live gaze arrow's (dx, dy) — see PoseWorker::gaze_ready — is a
/// face-box-relative offset roughly in [-1, 1] per axis (see
/// SkeletonOverlayW::paint_gaze() for the same convention used by the
/// post-hoc overlay). "On target" means the offset is small enough that the
/// subject reads as looking roughly toward the camera; threshold is a plain
/// radius in that same normalized space, not a status judgement — a subject
/// legitimately looking away isn't a system fault, so this returns a bool
/// for a neutral percentage stat, never a status-color tier.
inline constexpr double kGazeOnTargetThreshold = 0.35;

[[nodiscard]] bool gaze_on_target_for(double gazeDx, double gazeDy,
                                       double threshold = kGazeOnTargetThreshold);

/// @brief Buckets a remote-heart-rate (rPPG) window's pulse-SNR (dB, from
/// run_rppg.py's estimate_hr_welch()) into the same RmsQuality status
/// vocabulary already established for calibration quality and live
/// pose-tracking rate (pose_tracking_quality_for() above) — same reuse
/// rationale, not a new per-domain enum.
///
/// Forces Poor whenever validFrameFraction is below 0.6 (matching
/// run_rppg.py's own MIN_VALID_FRACTION gate) regardless of the reported
/// SNR — a quality read computed on sparse/mostly-missing face data isn't
/// meaningful even if the SNR number itself happens to look reasonable.
///
/// The SNR thresholds below are documented as heuristic defaults, not
/// derived from a specific calibration study — the same honest framing
/// already used for DetectionRateTracker's own bucket thresholds.
[[nodiscard]] RmsQuality rppg_quality_for(double snrDb, double validFrameFraction);

} // namespace mosaic
