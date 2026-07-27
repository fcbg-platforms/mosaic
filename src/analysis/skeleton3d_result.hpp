#pragma once
#include <QMap>
#include <QPair>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>
#include <cstdint>

namespace mosaic {

// Plain 3-vector (room-space mm) — std::array rather than QVector3D,
// matching CalibrationData's own std::array<double,N> convention for
// geometric data (src/core/settings.hpp) and GazeFusionResult's identical
// choice (src/analysis/gaze_fusion_result.hpp) — kept as a local alias
// here (not shared across the two headers) since a `using` declaration for
// the same underlying type is harmless to redeclare per translation unit,
// but a plain local alias avoids depending on that at all.
using Skeleton3DVec3 = std::array<double, 3>;

// Static per-camera room position (extrinsic_rt's translation column),
// written once per file — used by Skeleton3DRoomViewW's camera icons.
// Mirrors GazeFusionRoomCamera exactly.
struct Skeleton3DRoomCamera {
    int            index = -1;
    Skeleton3DVec3 positionRoom = {0, 0, 0};
};

// One triangulated keypoint within one reconstructed person. keypointsValid
// is the SINGLE source of truth for "meaningless" everywhere else in this
// struct — positionRoom/reprojectionErrorPx/every camera's entry in the
// owning Skeleton3DPerson::reprojectedPx are all left at their defaults
// (0/-1/absent) whenever !valid, never independently encoded.
struct Skeleton3DKeypoint {
    bool           valid = false;
    Skeleton3DVec3 positionRoom = {0, 0, 0};   // mm, room space
    double         reprojectionErrorPx = -1.0;  // -1 = n/a (!valid)
};

// One reconstructed 3D person within one fused frame. Mirrors
// run_pose3d.py's per-person JSON object.
struct Skeleton3DPerson {
    int    trackId = -1;
    int    numContributingCameras = 0;
    QVector<int> sourceCameras;
    QVector<Skeleton3DKeypoint> keypoints;   // same order/length as keypoint_names()
    // cameraIndex -> one reprojected 2D pixel point per keypoint (same
    // order/length as keypoints above), precomputed in Python for EVERY
    // calibrated camera (not just sourceCameras) — mirrors how
    // gaze_fusion.json precomputes face_box_px so the C++ overlay
    // (PoseOverlayPlayerW::set_skeleton3d_result()) never needs
    // calibration math at paint time. A keypoint index with
    // !keypoints[i].valid has no corresponding entry (a null QPointF, see
    // skeleton3d_result.cpp) in any camera's vector at that index.
    QMap<int, QVector<QPointF>> reprojectedPx;
};

// One fused master-tick. Mirrors run_pose3d.py's per-frame JSON object.
struct Skeleton3DFrame {
    int64_t tick        = 0;
    int64_t timestampNs = 0;
    QVector<Skeleton3DPerson> people;
};

// Parses a session-root "skeleton3d.json" file written by
// analysis/run_pose3d.py into a queryable in-memory structure, for the
// Analysis tab's 3D Pose Reconstruction plugin (per-camera reprojected
// skeleton overlay during playback, plus an interactive 3D room view).
// Mirrors GazeFusionResult (src/analysis/gaze_fusion_result.hpp) closely —
// same load()/is_valid() shape and the same "keyed on the shared master
// tick/timestamp, not any single video's frame_index" nearest-frame
// lookup, since reconstruction is inherently cross-camera. Diverges from
// GazeFusionResult in carrying MULTIPLE people per frame (gaze fusion only
// ever tracks one face) with a persistent trackId across frames.
//
// Usage:
//   auto result = Skeleton3DResult::load(jsonPath);
//   if (result.is_valid()) { ... }
class Skeleton3DResult {
public:
    Skeleton3DResult() = default;

    // Parses jsonPath. Returns a default-constructed (is_valid() == false)
    // result if the file is missing or malformed.
    static Skeleton3DResult load(const QString& jsonPath);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] const QStringList& source_videos() const { return sourceVideos_; }
    [[nodiscard]] const QVector<Skeleton3DRoomCamera>& cameras() const { return cameras_; }
    [[nodiscard]] const QStringList& keypoint_names() const { return keypointNames_; }
    [[nodiscard]] const QVector<QPair<int, int>>& skeleton_edges() const { return skeletonEdges_; }
    [[nodiscard]] double master_fps() const { return masterFps_; }
    [[nodiscard]] const QVector<Skeleton3DFrame>& frames() const { return frames_; }

    // Nearest-frame lookup by timestamp (ns) estimate — binary search,
    // since frames() is stored in the ascending timestampNs order
    // run_pose3d.py writes ascending master ticks in. Returns nullptr if
    // there are no frames.
    [[nodiscard]] const Skeleton3DFrame* nearest_frame(int64_t timestampNsEstimate) const;

private:
    bool        valid_ = false;
    QStringList sourceVideos_;
    QVector<Skeleton3DRoomCamera> cameras_;
    QStringList keypointNames_;
    QVector<QPair<int, int>> skeletonEdges_;
    double      masterFps_ = 25.0;
    QVector<Skeleton3DFrame> frames_;
};

} // namespace mosaic
