#pragma once
#include <QVector>
#include <cstdint>
#include <limits>

#include "analysis/skeleton3d_result.hpp"

namespace mosaic {

/// One derived sample of the relationship between two tracked people at one
/// fused tick (index-aligned with the source Skeleton3DResult::frames() —
/// one entry per tick, gaps left NaN rather than compacted away, unlike
/// pose_kinematics.hpp's compute_kinematics(), whose samples() only ever
/// holds valid entries). Each field is independently NaN whenever that
/// specific metric's own data requirement isn't met at this tick — never
/// fabricated/interpolated across a tracking gap (see
/// compute_dyadic_kinematics()'s own doc comment for exactly which
/// requirement gates which field).
struct DyadicSample {
    int64_t timestampNs = 0;
    /// mm, hip-midpoint (average of left_hip/right_hip) to hip-midpoint.
    double distanceMm = std::numeric_limits<double>::quiet_NaN();
    /// mm/s, real-Δt derivative of distanceMm between the two nearest valid
    /// distance samples. Negative = closing (approaching), positive =
    /// retreating.
    double approachRateMmPerS = std::numeric_limits<double>::quiet_NaN();
    /// [-1, 1] cosine similarity of the two people's torso-forward
    /// directions. -1 ≈ oriented oppositely (commonly face-to-face in a
    /// two-person interaction, but also consistent with two people standing
    /// back-to-back facing opposite compass directions — the metric is
    /// purely orientation-based, not position-aware, by construction);
    /// +1 ≈ oriented the same way; 0 ≈ perpendicular. See
    /// docs/math/dyadic_kinematics.rst for the full derivation.
    double facingCosine = std::numeric_limits<double>::quiet_NaN();
    /// [-1, 1] trailing-window Pearson correlation of the two people's own
    /// instantaneous hip-midpoint speed (each computed independently, real-Δt
    /// between that person's own nearest valid samples). NaN until at least
    /// 3 paired (both people's speed defined at that tick) samples exist
    /// within the window.
    double congruentMotionCorr = std::numeric_limits<double>::quiet_NaN();
};

/// Summary statistics over one compute_dyadic_kinematics() call's samples.
/// Each field is NaN if no sample ever populated the corresponding
/// DyadicSample field (e.g. meanFacingCosine stays NaN for a session whose
/// Skeleton3DResult::keypoint_names() lacks "nose"/"left_shoulder"/
/// "right_shoulder").
struct DyadicStats {
    double meanDistanceMm          = std::numeric_limits<double>::quiet_NaN();
    double minDistanceMm           = std::numeric_limits<double>::quiet_NaN();
    double maxDistanceMm           = std::numeric_limits<double>::quiet_NaN();
    double meanFacingCosine        = std::numeric_limits<double>::quiet_NaN();
    double meanCongruentMotionCorr = std::numeric_limits<double>::quiet_NaN();
    /// 0-100. % of the result's total tick count with both people's hips
    /// valid at that tick (i.e. distanceMm is defined there).
    double pctTicksBothPresent = std::numeric_limits<double>::quiet_NaN();
};

/// Output of compute_dyadic_kinematics() — one sample per tick plus summary
/// stats over the whole result.
struct DyadicKinematicsSeries {
    QVector<DyadicSample> samples;
    DyadicStats stats;
};

/// Derives interpersonal distance, approach/retreat rate, torso-facing
/// similarity, and movement-synchrony (congruent motion) between two
/// already-reconstructed people (by trackId) from an already-loaded
/// Skeleton3DResult — a pure, plain-data-in/plain-data-out function, no
/// dependency on any live manager/Qt-widget object, matching this project's
/// established "isolate the derived-math, make it directly unit-testable"
/// discipline (e.g. pose_kinematics.hpp's compute_kinematics()).
///
/// Requires the result's keypoint_names() to contain "left_hip"/"right_hip"
/// for distance/approach-rate to compute at all; additionally
/// "nose"/"left_shoulder"/"right_shoulder" for facingCosine — a result
/// missing the latter three still computes distance/approach-rate normally,
/// with facingCosine (and its stats.meanFacingCosine) staying permanently
/// NaN, rather than crashing or silently misreading the wrong index.
/// Indices are resolved once by name (Skeleton3DResult::keypoint_names()),
/// never a hardcoded COCO index.
///
/// @param congruentMotionWindow Trailing window size (in paired-valid
///     samples, i.e. ticks where both people's own hip-midpoint speed is
///     defined) for the rolling Pearson correlation.
///
/// @see docs/math/dyadic_kinematics.rst for the full facing-direction
///      cross-product derivation and its nose-relative sign resolution
///      (self-correcting per person per frame — makes the raw cross
///      product's argument order irrelevant), and the congruent-motion
///      correlation formula.
[[nodiscard]] DyadicKinematicsSeries compute_dyadic_kinematics(const Skeleton3DResult& result,
                                                               int trackIdA, int trackIdB,
                                                               int congruentMotionWindow = 30);

} // namespace mosaic
