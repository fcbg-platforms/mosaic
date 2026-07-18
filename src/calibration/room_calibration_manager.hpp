#pragma once
#include "calibration/room_frame_solver.hpp"
#include "core/settings.hpp"
#include "video/video_frame.hpp"
#include <QVector>
#include <array>
#include <memory>

namespace mosaic {

/// @brief Multi-camera extrinsic ("room") calibration via a shared ChArUco board.
///
/// Companion to CalibrationManager, which only handles single-camera
/// intrinsics. A camera must already have a valid intrinsic CalibrationData
/// (set_camera_intrinsics()) before its views can contribute to a solve —
/// cv::solvePnP() needs real, non-identity intrinsics to produce a
/// meaningful board pose.
///
/// Requires @c MOSAIC_HAVE_OPENCV — is_available() returns @c false otherwise.
///
/// @par Workflow
/// @code{.cpp}
/// RoomCalibrationManager mgr;
/// mgr.set_board({7, 5, 40.0, 30.0});
/// for (int i = 0; i < cameraCount; ++i) mgr.set_camera_intrinsics(i, settings.cameras[i].calibration);
///
/// // Repeat, moving the board through overlapping pairs of camera views:
/// mgr.feed_shot(simultaneousFrames);
///
/// const auto result = mgr.solve(cameraCount, /*referenceCameraIndex=*/0);
/// if (mgr.is_resolved(2)) { auto rt = mgr.extrinsic_for(2); ... }
///
/// std::array<double,3> planePoint, planeNormal;
/// mgr.use_shot_as_plane(lastShotIndex, 0, planePoint, planeNormal);
/// @endcode
class RoomCalibrationManager {
public:
    /// @brief ChArUco board geometry.
    struct BoardSpec {
        int    cols           = 7;      ///< Number of squares, horizontally.
        int    rows           = 5;      ///< Number of squares, vertically.
        double squareLengthMm = 40.0;   ///< Physical side length of one square, in mm.
        double markerLengthMm = 30.0;   ///< Physical side length of one ArUco marker, in mm.
    };

    /// @brief Outcome of one camera's board detection within a single shot.
    struct CameraShotResult {
        int  cameraIndex = -1;
        bool found        = false;   ///< true if enough ChArUco corners were found to solve a pose.
        int  cornerCount  = 0;
    };

    /// @brief Outcome of a solve() call.
    struct SolveResult {
        std::vector<bool>   resolved;            ///< index = cameraIndex
        std::vector<double> reprojectionRmsPx;    ///< index = cameraIndex; -1 = never directly seen
    };

    RoomCalibrationManager();
    ~RoomCalibrationManager();

    [[nodiscard]] static bool is_available();

    /// Call before the first feed_shot(). Changing the spec after capturing
    /// shots requires clear_shots() to discard incompatible data.
    void set_board(const BoardSpec& spec);

    /// Registers a camera's already-computed intrinsic calibration (from
    /// CalibrationManager). Must be called once per camera before that
    /// camera's frames in feed_shot() can be used — a camera with
    /// @c !data.calibrated is skipped by feed_shot() with a warning.
    void set_camera_intrinsics(int cameraIndex, const CalibrationData& data);

    /// Runs ChArUco detection + per-camera cv::solvePnP() on one
    /// simultaneous multi-camera shot. A camera missing from @p frames (the
    /// UI's per-shot capture timed out waiting for it) or lacking prior
    /// intrinsics is simply absent from the accepted shot data, reported as
    /// `found = false` here. Returns one result per input frame, in order.
    QVector<CameraShotResult> feed_shot(const QVector<VideoFrame>& frames);

    [[nodiscard]] int shot_count() const;
    void clear_shots();

    /// Resolves every camera's extrinsicRt (pose relative to
    /// referenceCameraIndex, which always resolves to identity) by BFS over
    /// the shared-shot graph accumulated so far (room_frame::bfs_resolve()).
    /// Also computes each directly-observed camera's own reprojection RMS
    /// (its solvePnP fit quality, independent of any BFS chain length).
    SolveResult solve(int cameraCount, int referenceCameraIndex);

    [[nodiscard]] bool                   is_resolved(int cameraIndex) const;
    [[nodiscard]] std::array<double, 16> extrinsic_for(int cameraIndex) const;
    [[nodiscard]] double                 reprojection_rms_for(int cameraIndex) const;

    /// Uses the board's own pose from a specific (shot, camera) pair — where
    /// the board was presumably lying flat on the target surface — as the
    /// room's reference plane. Both the shot's camera detection and that
    /// camera's own extrinsic resolution (solve() already called) must
    /// exist. outNormal is the board's printed-face normal (its local Z
    /// axis) in room coordinates. Returns false if no matching detection
    /// exists or the camera is unresolved.
    [[nodiscard]] bool use_shot_as_plane(int shotIndex, int cameraIndex,
                                          std::array<double, 3>& outPoint,
                                          std::array<double, 3>& outNormal) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
