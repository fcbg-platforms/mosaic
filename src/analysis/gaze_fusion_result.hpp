#pragma once
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>
#include <cstdint>

namespace mosaic {

/// Plain 3-vector (room-space mm) — std::array rather than QVector3D
/// deliberately, matching CalibrationData's own std::array<double,N>
/// convention for geometric data (src/core/settings.hpp) and keeping this
/// class free of the QtGui dependency QVector3D would pull in.
using Vec3 = std::array<double, 3>;

/// One camera's view of one subject at one tick.
struct GazeFusionCamera {
    int cameraIndex = -1;
    QRectF faceBoxPx;           ///< face_box_px, that camera's video pixel space.
    bool hasDirection  = false; ///< This camera contributed to the fused gaze.
    Vec3 directionRoom = {0, 0, 0};
    double weight      = 0.0; ///< How much this camera counted (0..2, two eyes).
};

/// One subject at one tick: where the gaze comes from, where it goes, and
/// what it lands on.
struct GazeSubjectSample {
    QString id;   ///< "S1", "S2"...
    QString name; ///< Display name (renamed via gaze_targets.json).
    Vec3 origin       = {0, 0, 0};
    bool hasDirection = false; ///< false: head seen but no usable eyes.
    Vec3 direction    = {0, 0, 0};
    bool hasPoint     = false;
    Vec3 point        = {0, 0, 0}; ///< The 3D gaze point.
    QString targetType;            ///< "subject", "region", "plane", "none" or empty.
    QString targetLabel;           ///< Display label of the target.
    QString targetSubject;         ///< Looked-at subject id when targetType == "subject".
    bool mutual           = false;
    double confidence     = 0.0;
    double uncertaintyDeg = -1.0; ///< -1 = unknown.
    int numCameras        = 0;
    QVector<GazeFusionCamera> perCamera;
};

/// Static per-camera room position (extrinsic_rt's translation column),
/// written once per file — used by the room-view widget's camera icons.
struct GazeFusionRoomCamera {
    int index         = -1;
    Vec3 positionRoom = {0, 0, 0};
};

/// A named rectangle in the room that gaze can land on.
struct GazeFusionRegion {
    QString name;
    Vec3 centre   = {0, 0, 0};
    Vec3 normal   = {0, 0, 1};
    Vec3 uAxis    = {1, 0, 0};
    double width  = 0.0;
    double height = 0.0;
};

/// One analysed tick.
struct GazeFusionFrame {
    int64_t tick            = 0;
    int64_t timestampNs     = 0;
    int64_t videoFrameIndex = -1; ///< Frame in synced/ and gaze_fusion/ videos; -1 in v1 files.
    QVector<GazeSubjectSample> subjects;

    /// The sample of subject `id`, or nullptr.
    [[nodiscard]] const GazeSubjectSample* subject(const QString& id) const;
};

/// Parses a session-root "gaze_fusion.json" written by
/// analysis/run_gaze_fusion.py into a queryable in-memory structure, for the
/// Analysis tab's Multi-Camera Gaze Fusion page (annotated videos or a live
/// overlay, plus the top-down room view).
///
/// Reads the current schema (mosaic-gaze-fusion-v2: several subjects per
/// tick, targets, mutual gaze) and the original single-subject v1 files,
/// which load as one subject "S1" so every view handles both the same way.
///
/// Usage:
/// @code
///   auto result = GazeFusionResult::load(jsonPath);
///   if (result.is_valid()) { ... }
/// @endcode
class GazeFusionResult {
   public:
    GazeFusionResult() = default;

    /// Parses jsonPath. Returns a default-constructed (is_valid() == false)
    /// result if the file is missing or malformed.
    static GazeFusionResult load(const QString& jsonPath);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] int schema_version() const { return schemaVersion_; }
    [[nodiscard]] const QStringList& source_videos() const { return sourceVideos_; }
    /// Annotated video per camera index, relative to the session; empty when
    /// none was rendered (v1 files, or a run with --no-render).
    [[nodiscard]] QString annotated_video(int cameraIndex) const;
    [[nodiscard]] const QString& room_video() const { return roomVideo_; }
    [[nodiscard]] const QVector<GazeFusionRoomCamera>& cameras() const { return cameras_; }
    [[nodiscard]] bool plane_defined() const { return planeDefined_; }
    [[nodiscard]] Vec3 plane_point() const { return planePoint_; }
    [[nodiscard]] Vec3 plane_normal() const { return planeNormal_; }
    [[nodiscard]] const QVector<GazeFusionRegion>& regions() const { return regions_; }
    /// Subject ids in order, with their display names.
    [[nodiscard]] const QStringList& subject_ids() const { return subjectIds_; }
    [[nodiscard]] QString subject_name(const QString& id) const;
    [[nodiscard]] double master_fps() const { return masterFps_; }
    [[nodiscard]] const QVector<GazeFusionFrame>& frames() const { return frames_; }

    /// Nearest-frame lookup by timestamp (ns) — binary search over frames()
    /// in ascending timestamp order. Returns nullptr if there are no frames.
    [[nodiscard]] const GazeFusionFrame* nearest_frame(int64_t timestampNsEstimate) const;

    /// The frame to show at a frame of the synced/annotated videos: the
    /// latest analysed frame at or before it, within `holdFrames` (analysis
    /// runs on every Nth tick). nullptr when none is that recent, or for v1
    /// files, which have no video frame index.
    [[nodiscard]] const GazeFusionFrame* frame_for_video_frame(int64_t videoFrameIndex,
                                                               int64_t holdFrames) const;

    /// The frame to show at a playback position (ms from the start of the
    /// video). For v2 results analysed on synced/ videos, and for the
    /// annotated videos: exact, via the video frame index at master_fps(),
    /// holding each analysed frame for max(analysed_every(), 0.3 s worth of
    /// frames). For v1 results and raw-video sources, which are not one frame
    /// per tick: approximate, by time from the first analysed tick.
    [[nodiscard]] const GazeFusionFrame* frame_at_position_ms(int64_t positionMs) const;

    /// Analysis ran on every Nth tick (1 for v1 files).
    [[nodiscard]] int analysed_every() const { return analysedEvery_; }

   private:
    bool valid_        = false;
    int schemaVersion_ = 0;
    QStringList sourceVideos_;
    QVector<QPair<int, QString>> annotatedVideos_;
    QString roomVideo_;
    QString source_;
    QVector<GazeFusionRoomCamera> cameras_;
    bool planeDefined_ = false;
    Vec3 planePoint_   = {0, 0, 0};
    Vec3 planeNormal_  = {0, 0, 1};
    QVector<GazeFusionRegion> regions_;
    QStringList subjectIds_;
    QStringList subjectNames_;
    double masterFps_  = 25.0;
    int analysedEvery_ = 1;
    QVector<GazeFusionFrame> frames_;
};

} // namespace mosaic
