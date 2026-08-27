#pragma once
#include <QVector>
#include <cstdint>
#include <limits>

#include "analysis/skeleton3d_result.hpp"

namespace mosaic {

/// @brief One derived sample of the relationship between two tracked people
/// at one fused tick.
///
/// Fields are independently NaN whenever that specific metric's own data
/// requirement isn't met at this tick (see compute_dyadic_kinematics()) —
/// never fabricated/interpolated across a tracking gap.
struct DyadicSample {
    int64_t timestampNs = 0;
    double distanceMm = std::numeric_limits<double>::quiet_NaN(); ///< hip-midpoint to hip-midpoint
    double approachRateMmPerS = std::numeric_limits<double>::quiet_NaN(); ///< negative = closing
    double facingCosine       = std::numeric_limits<double>::quiet_NaN(); ///< see class doc below
    double congruentMotionCorr =
        std::numeric_limits<double>::quiet_NaN(); ///< rolling Pearson r of speed
};

/// @brief Summary statistics over one compute_dyadic_kinematics() call's samples.
struct DyadicStats {
    double meanDistanceMm          = std::numeric_limits<double>::quiet_NaN();
    double minDistanceMm           = std::numeric_limits<double>::quiet_NaN();
    double maxDistanceMm           = std::numeric_limits<double>::quiet_NaN();
    double meanFacingCosine        = std::numeric_limits<double>::quiet_NaN();
    double meanCongruentMotionCorr = std::numeric_limits<double>::quiet_NaN();
    /// Fraction (0-1, not already scaled to %) of the result's own tick
    /// range where both A and B have a valid hip midpoint — i.e. where
    /// distanceMm is computable at all. 0 if the result has no frames.
    double pctTicksBothPresent = 0.0;
};

/// @brief Output of compute_dyadic_kinematics() — per-tick series plus summary stats.
struct DyadicKinematicsSeries {
    /// One entry per tick in the source Skeleton3DResult, in the same
    /// order — gaps are left with NaN fields, never skipped, so a caller
    /// can see exactly where data was and wasn't available.
    QVector<DyadicSample> samples;
    DyadicStats stats;
};

/// @brief Derives interpersonal distance/approach-rate/facingness/congruent-motion
/// between two already-reconstructed people (by trackId) from an
/// already-loaded Skeleton3DResult (the 3D Pose Reconstruction plugin's own
/// session-level result).
///
/// Requires the result's keypoint_names() to contain "nose",
/// "left_shoulder", "right_shoulder", "left_hip", and "right_hip" (resolved
/// once by name, not a hardcoded COCO index) — facingCosine stays
/// permanently NaN for every sample (distanceMm/approachRateMmPerS still
/// compute normally) if any of the five is missing, rather than crashing or
/// silently reading the wrong index on a non-COCO skeleton.
///
/// Distance/approach-rate use each person's hip midpoint
/// (average of their left_hip/right_hip positionRoom). Facingness needs the
/// same hip midpoint plus the shoulder midpoint and nose, per person — see
/// the .cpp's facing_vector() for the full derivation: a "chest-forward"
/// unit vector from cross(spine, shoulders), with its inherent front/back
/// sign ambiguity resolved by checking which sign points toward that
/// person's own nose. facingCosine is the dot product of the two people's
/// forward vectors: near -1 when oriented in opposite directions (commonly
/// — but not provably, since this is a position-independent orientation
/// comparison — face-to-face in a two-person interaction; also consistent
/// with standing back-to-back facing directly away from each other), near
/// +1 when oriented the same way (e.g. side-by-side), near 0 when
/// perpendicular. This limitation is documented, not a bug: the metric is a
/// pure torso-heading correlation, not a "looking at each other" detector.
///
/// Approach rate and each person's own speed (used internally for
/// congruent motion) both follow pose_kinematics.hpp's established
/// discipline exactly: skip missing samples rather than interpolate, and
/// derive using the REAL elapsed time between the two nearest valid
/// samples, so a tracking gap yields an honest average rather than a
/// fabricated spike.
///
/// @param congruentMotionWindow Trailing window size (in paired valid speed
///     samples, not ticks) for the rolling Pearson correlation of A's and
///     B's instantaneous speed. A tick's congruentMotionCorr stays NaN
///     until at least 3 paired samples exist in its trailing window, and
///     also if either signal has zero variance in that window (e.g. one
///     person standing still — not an error, just undefined correlation).
[[nodiscard]] DyadicKinematicsSeries compute_dyadic_kinematics(const Skeleton3DResult& result,
                                                               int trackIdA, int trackIdB,
                                                               int congruentMotionWindow = 30);

} // namespace mosaic
