#pragma once
#include <QPair>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>

namespace mosaic {

/// Identifies *which person* a query is about, as opposed to where they
/// happened to sit in a frame's subjects array.
///
/// A distinct type on purpose. Before tracking existed, run_pose.py wrote
/// subject_id == array position, so "subject index" and "subject id" were the
/// same int holding the same value — meaning a missed call site during the
/// switch to identity keying would have compiled cleanly and silently plotted
/// a different person. Making it its own type turns every such site into a
/// compile error instead.
struct SubjectId {
    int value = -1;

    SubjectId() = default;
    explicit constexpr SubjectId(int v) : value(v) {}

    /// Untracked detections carry a negative id (run_pose.py's -(i+1)
    /// fallback); real tracker ids are >= 0.
    [[nodiscard]] constexpr bool is_tracked() const { return value >= 0; }

    friend constexpr bool operator==(SubjectId, SubjectId) = default;
};

/// One detected subject's keypoints within a single analysed frame.
/// Mirrors run_pose.py's _result_to_dict() "subjects" entries exactly.
struct PoseSubject {
    int subjectId     = -1;
    double confidence = 0.0;
    QVector<QPointF> keypoints;   ///< Pixel coordinates, one per keypoint name.
    QVector<double> visibilities; ///< 0-1, same order as keypoints.
    QRectF bbox;                  ///< bbox_xyxy.
};

/// Shared visibility threshold for "trust this keypoint" decisions — used
/// both to decide whether to draw a keypoint (SkeletonOverlayW::paintEvent,
/// src/ui/analysis/pose_overlay_player_w.cpp) and whether to include it in
/// derived kinematics (pose_kinematics.cpp). Kept in one place so the two
/// can't silently drift to different thresholds. A missing visibilities
/// entry (index >= visibilities.size()) defaults to "visible" — matches
/// keypoints[] always being written 1:1 with visibilities[] by run_pose.py,
/// so this only matters for a malformed/truncated file, where treating an
/// unknown entry as visible is the same permissive default this codebase
/// already used before this helper existed.
inline bool is_keypoint_visible(const PoseSubject& subject, int keypointIndex) {
    return subject.visibilities.value(keypointIndex, 1.0) >= 0.1;
}

/// One analysed frame. Mirrors run_pose.py's per-frame JSON object.
struct PoseFrame {
    int frameIndex      = 0;
    int64_t timestampNs = 0;
    int cameraIndex     = 0;
    QVector<PoseSubject> subjects;
};

/// The subject carrying `id` in this frame, or nullptr if that person wasn't
/// detected here. A linear scan: a frame holds a handful of subjects, so a
/// per-frame index would cost more to build than it saves.
///
/// Returning nullptr for "not in this frame" is the whole point of identity
/// keying — the old positional lookup could not tell "this person is missing"
/// apart from "this array is shorter", and silently borrowed whoever occupied
/// that index instead.
///
/// First match wins if a file somehow repeats an id within one frame.
/// run_pose.py cannot produce that (tracker ids are unique per frame and the
/// untracked fallback is -(i+1)), so it only arises in a hand-edited or
/// third-party file, where deterministic beats clever.
[[nodiscard]] inline const PoseSubject* find_subject(const PoseFrame& frame, SubjectId id) {
    for (const auto& subject : frame.subjects) {
        if (subject.subjectId == id.value) {
            return &subject;
        }
    }
    return nullptr;
}

/// Parses a .pose.json file written by analysis/run_pose.py (--out-format json)
/// into a queryable in-memory structure, for drawing a live overlay during
/// playback and plotting per-keypoint metrics over time.
///
/// Usage:
/// @code
///   auto result = PoseAnalysisResult::load(jsonPath);
///   if (result.is_valid()) { ... }
/// @endcode
class PoseAnalysisResult {
   public:
    PoseAnalysisResult() = default;

    /// Parses jsonPath. Returns a default-constructed (is_valid() == false)
    /// result if the file is missing or malformed.
    static PoseAnalysisResult load(const QString& jsonPath);

    [[nodiscard]] bool is_valid() const { return valid_; }

    /// True if at least one frame has at least one detected subject.
    /// Distinct from is_valid() (which only means "the JSON parsed"): a
    /// result can be valid but empty if the pose model detected nobody in
    /// this camera's footage for the whole session — callers use this to
    /// tell that case apart from "hasn't been analyzed yet".
    [[nodiscard]] bool has_any_detections() const {
        for (const auto& f : frames_) {
            if (!f.subjects.isEmpty()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const QString& source_video() const { return sourceVideo_; }
    /// The model id that produced this result (e.g. "yolov8n-pose.pt"), from
    /// the JSON's top-level "model" field — empty for files written before
    /// this field existed (older, un-namespaced .pose.json files still load
    /// fine, they just report no model).
    [[nodiscard]] const QString& model() const { return model_; }

    /// The cross-frame tracker that produced this result's subject ids (e.g.
    /// "botsort"), from the JSON's top-level "tracker" field. Empty for files
    /// written before tracking existed, whose subject_id is merely per-frame
    /// detection order. Same absent-field convention as model() above.
    [[nodiscard]] const QString& tracker() const { return tracker_; }

    /// Whether this result's subject ids mean "the same physical person"
    /// across frames. Drives the UI's caveat wording: a pre-tracking file must
    /// keep the old "detection order, not a tracked individual" warning, and
    /// inferring that from the ids themselves would be guesswork.
    [[nodiscard]] bool has_tracked_identity() const { return !tracker_.isEmpty(); }

    /// The subject ids that can be followed across frames, ascending.
    /// Computed once at load().
    ///
    /// Excludes untracked (negative) ids, which run_pose.py assigns per frame
    /// and which therefore identify a different person from one frame to the
    /// next — see collect_subject_ids(). Use has_untracked_detections() to
    /// tell whether any were left out.
    ///
    /// Replaces the old "widest subjects array in any frame" count. For a
    /// pre-tracking file — dense ids equal to array positions — this yields
    /// {0, 1, ... N-1}, i.e. exactly the old chip order, so legacy results
    /// look and behave identically.
    [[nodiscard]] const QVector<SubjectId>& subject_ids() const { return subjectIds_; }

    /// Whether the file holds detections the tracker never claimed. Those are
    /// absent from subject_ids() (and so from the chips, chart and export),
    /// but still drawn on the video overlay.
    [[nodiscard]] bool has_untracked_detections() const { return hasUntrackedDetections_; }

    [[nodiscard]] const QStringList& keypoint_names() const { return keypointNames_; }
    [[nodiscard]] const QVector<QPair<int, int>>& skeleton_edges() const { return skeletonEdges_; }
    [[nodiscard]] const QVector<PoseFrame>& frames() const { return frames_; }

    /// Nearest-frame lookup by frame_index estimate (e.g. derived from a
    /// video playback position). frames() is stored in the ascending
    /// frame_index order run_pose.py writes them in, so this is a binary
    /// search, not a linear scan. Returns nullptr if there are no frames.
    [[nodiscard]] const PoseFrame* nearest_frame(int frameIndexEstimate) const;

   private:
    bool valid_ = false;
    QString sourceVideo_;
    QString model_;
    QString tracker_;
    QStringList keypointNames_;
    QVector<QPair<int, int>> skeletonEdges_;
    QVector<PoseFrame> frames_;
    QVector<SubjectId> subjectIds_;
    bool hasUntrackedDetections_ = false;
};

} // namespace mosaic
