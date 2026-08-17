#pragma once
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>

namespace mosaic {

/// One detected face's blendshape scores and classified expression within a
/// single analysed frame. Mirrors run_expression.py's _frame_to_dict()
/// "subjects" entries exactly.
struct ExpressionSubject {
    int subjectId     = -1;
    double confidence = 0.0;
    QRectF bbox;                      ///< bbox_xyxy.
    QVector<double> blendshapeScores; ///< Parallel to ExpressionResult::blendshape_names().
    QString dominantExpression;
    double dominantScore = 0.0;
    /// FACS Action Unit intensities (py-feat backend only), each in [0,1],
    /// parallel to ExpressionResult::au_names(). Empty for the heuristic/
    /// FER+ backends and for any session analyzed before this field existed.
    QVector<double> actionUnits;
};

/// One analysed frame. Mirrors run_expression.py's per-frame JSON object.
struct ExpressionFrame {
    int frameIndex      = 0;
    int64_t timestampNs = 0;
    int cameraIndex     = 0;
    QVector<ExpressionSubject> subjects;
};

/// Parses a "<name>.expression.json" file written by
/// analysis/run_expression.py into a queryable in-memory structure, for the
/// Analysis tab's Facial Expression plugin (bbox+label overlay during
/// playback and a per-blendshape score-over-time plot). Mirrors
/// PoseAnalysisResult (src/analysis/pose_analysis_result.hpp) closely — same
/// load()/is_valid()/nearest_frame() shape.
///
/// Usage:
/// @code
///   auto result = ExpressionResult::load(jsonPath);
///   if (result.is_valid()) { ... }
/// @endcode
class ExpressionResult {
   public:
    ExpressionResult() = default;

    /// Parses jsonPath. Returns a default-constructed (is_valid() == false)
    /// result if the file is missing or malformed.
    static ExpressionResult load(const QString& jsonPath);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] const QString& source_video() const { return sourceVideo_; }
    [[nodiscard]] const QString& backend() const { return backend_; }
    [[nodiscard]] const QStringList& blendshape_names() const { return blendshapeNames_; }
    [[nodiscard]] const QVector<ExpressionFrame>& frames() const { return frames_; }

    /// Action Unit names (py-feat backend only) — empty for the heuristic/
    /// FER+ backends and for older files with no "au_names" field at all.
    [[nodiscard]] const QStringList& au_names() const { return auNames_; }
    [[nodiscard]] bool has_action_units() const { return !auNames_.isEmpty(); }

    /// True if at least one frame has at least one detected face. Distinct
    /// from is_valid() (which only means "the JSON parsed"): a result can be
    /// valid but empty if no face was detected in this camera's footage for
    /// the whole session — mirrors PoseAnalysisResult::has_any_detections().
    [[nodiscard]] bool has_any_detections() const {
        for (const auto& f : frames_) {
            if (!f.subjects.isEmpty()) {
                return true;
            }
        }
        return false;
    }

    /// Nearest-frame lookup by frame_index estimate, identical body to
    /// PoseAnalysisResult::nearest_frame() — frames() is stored in the
    /// ascending frame_index order run_expression.py writes them in, so this
    /// is a binary search, not a linear scan. Returns nullptr if there are
    /// no frames.
    [[nodiscard]] const ExpressionFrame* nearest_frame(int frameIndexEstimate) const;

   private:
    bool valid_ = false;
    QString sourceVideo_;
    QString backend_;
    QStringList blendshapeNames_;
    QStringList auNames_;
    QVector<ExpressionFrame> frames_;
};

} // namespace mosaic
