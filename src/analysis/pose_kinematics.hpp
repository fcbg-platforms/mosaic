#pragma once
#include "analysis/pose_analysis_result.hpp"
#include <QPointF>
#include <QVector>
#include <cstdint>
#include <limits>

namespace mosaic {

/// @brief One derived sample along a single keypoint's trajectory.
///
/// speedPxPerS is NaN for the first valid sample (no prior point to diff
/// against); accelPxPerS2 is NaN for the first two.
struct KinematicSample {
    int64_t timestampNs  = 0;
    QPointF position;      ///< px, post-smoothing
    double  speedPxPerS  = std::numeric_limits<double>::quiet_NaN();
    double  accelPxPerS2 = std::numeric_limits<double>::quiet_NaN();
};

/// @brief Summary statistics over one compute_kinematics() call's samples.
struct KinematicStats {
    double totalDistancePx = 0.0;
    double avgSpeedPxPerS  = std::numeric_limits<double>::quiet_NaN(); ///< NaN if <2 valid samples
    double maxSpeedPxPerS  = std::numeric_limits<double>::quiet_NaN();
};

/// @brief Output of compute_kinematics() — per-sample series plus summary stats.
struct KinematicsSeries {
    QVector<KinematicSample> samples;
    KinematicStats            stats;
};

/// @brief Derives speed/acceleration for one keypoint of one subject from an
/// already-loaded PoseAnalysisResult.
///
/// Frames where the subject/keypoint is missing or has visibility < 0.1
/// (matching SkeletonOverlayW's paint threshold) are skipped rather than
/// interpolated — velocity across the resulting gap uses the real elapsed
/// time between the two nearest valid samples, so a detection dropout
/// yields an honest average speed across it instead of a value fabricated
/// from an assumed frame rate.
///
/// @param smoothingWindow Number of samples in a centered moving average
///     applied to position before differentiating (1 = no smoothing). Even
///     values are treated as the next-lower odd width (a centered window
///     needs a middle sample); the UI is expected to only offer odd
///     values, but this function stays safe if called directly with an
///     even one.
///
/// @see docs/math/pose_kinematics.rst for the full derivation, including
///      why avgSpeedPxPerS is time-weighted rather than a naive mean of
///      per-interval speeds.
[[nodiscard]] KinematicsSeries compute_kinematics(const PoseAnalysisResult& result,
                                                    int keypointIndex, int subjectIndex,
                                                    int smoothingWindow);

} // namespace mosaic
