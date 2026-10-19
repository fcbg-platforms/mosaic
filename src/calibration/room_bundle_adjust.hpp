#pragma once

#if defined(MOSAIC_HAVE_OPENCV)
#include <map>
#include <opencv2/core.hpp>
#include <vector>

#include "calibration/room_frame_solver.hpp"

namespace mosaic::room_ba {

// ── Bundle adjustment of the room calibration ───────────────────────────────
//
// bfs_resolve() chains cameras together through averaged pairwise board
// poses: each camera's pose rests on one link, and the errors of a chain add
// up. Bundle adjustment then refines everything at once: every camera's pose
// and every shot's board pose are adjusted together so that each board
// corner, seen by every camera that saw it, reprojects where it was detected.
// The reference camera stays at the origin (it fixes the room's frame) and
// the board's printed size fixes the scale; intrinsics are held fixed.

using room_frame::Mat4;

/// One camera's view of the board in one shot.
struct Observation {
    int shot   = -1;
    int camera = -1;
    std::vector<cv::Point3f> object;          ///< board corners, board frame, mm
    std::vector<cv::Point2f> image;           ///< where they were detected, px
    Mat4 boardToCam = room_frame::identity(); ///< this camera's own PnP pose
};

/// A camera's fixed lens.
struct Lens {
    cv::Matx33d K;
    cv::Mat dist;
};

/// The outcome of refine().
struct Result {
    /// True when the refined poses were accepted (they fit better and no
    /// camera moved implausibly far); else cameraToRoom is the input.
    bool applied = false;
    std::vector<Mat4> cameraToRoom; ///< per camera index, as the input
    /// Per camera: RMS reprojection error of its corners against the room
    /// model (shared board poses), before and after, px; -1 = not used. Over
    /// the views kept: a view left out (droppedPerCamera) is not in it.
    std::vector<double> rmsBeforePx;
    std::vector<double> rmsAfterPx;
    /// Per camera: how far refinement moved it, mm and degrees.
    std::vector<double> movedMm;
    std::vector<double> movedDeg;
    double totalRmsBeforePx = -1.0;
    double totalRmsAfterPx  = -1.0;
    int shotsUsed           = 0;
    int cornersUsed         = 0;
    /// Corners reprojecting worse than kOutlierPx after refinement: they
    /// were down-weighted, not trusted.
    int outlierCorners = 0;
    /// Views (one camera, one shot) left out for fitting far worse than the
    /// rest after a first refinement.
    int droppedViews = 0;
    std::vector<int> droppedPerCamera; ///< the same, per camera index
    int iterations = 0;
};

/// Corners off by more than this count less (Huber weighting), px.
inline constexpr double kHuberPx = 2.0;
/// A corner off by more than this after refinement is reported as an outlier.
inline constexpr double kOutlierPx = 6.0;
/// A view whose RMS is over this and over 4 times the median view's is
/// left out and the refinement run again, px.
inline constexpr double kBadViewPx = 3.0;
/// Refinement moving any camera further than this is not trusted (the
/// starting poses are a few mm off at most; much more means the problem was
/// ill-posed): the input poses are kept.
inline constexpr double kMaxMoveMm  = 150.0;
inline constexpr double kMaxMoveDeg = 8.0;

/// Refines @p cameraToRoom (camera to room, as CalibrationData::extrinsicRt)
/// for the cameras marked @p resolved, from every shot that two or more
/// resolved cameras saw. @p referenceIndex is held at its input pose.
[[nodiscard]] Result refine(int cameraCount, int referenceIndex, const std::vector<bool>& resolved,
                            const std::vector<Mat4>& cameraToRoom,
                            const std::map<int, Lens>& lenses,
                            const std::vector<Observation>& observations);

} // namespace mosaic::room_ba

#endif
