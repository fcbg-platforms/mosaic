#pragma once
#include <QRect>
#include <QString>
#include <QVector>
#include <cstdint>
#include <optional>

namespace mosaic {

/// One processed video frame's face detection + calibration-free 2D gaze
/// heuristic. Mirrors run_gaze2d.py's "frames" JSON array exactly.
/// faceDetected == false means faceBoxPx/gazeDx/gazeDy are meaningless
/// (default-constructed, not real values) — the single source of truth
/// callers must check, matching every sibling result class's own
/// "one boolean gates several parallel fields" convention.
struct Gaze2dFrame {
    int frameIndex      = 0;
    int64_t timestampMs = 0;
    bool faceDetected   = false;
    QRect faceBoxPx;
    double gazeDx = 0.0;
    double gazeDy = 0.0;
};

/// Parses a "<video_stem>.gaze2d.json" file written by
/// analysis/run_gaze2d.py into a queryable in-memory structure, for the
/// Analysis tab's 2D Gaze (calibration-free) plugin.
///
/// Usage:
/// @code
///   auto result = Gaze2dResult::load(jsonPath);
///   if (result.is_valid()) { ... }
/// @endcode
class Gaze2dResult {
   public:
    Gaze2dResult() = default;

    /// Parses jsonPath. Returns a default-constructed (is_valid() == false)
    /// result if the file is missing or malformed.
    static Gaze2dResult load(const QString& jsonPath);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] const QString& source_video() const { return sourceVideo_; }
    [[nodiscard]] double min_confidence() const { return minConfidence_; }
    [[nodiscard]] const QVector<Gaze2dFrame>& frames() const { return frames_; }

    /// Precomputed by run_gaze2d.py from the raw per-frame gaze values —
    /// not recomputed here, avoiding duplicating aggregate math in two
    /// languages. std::nullopt if no frame in the file had a detected face.
    [[nodiscard]] double pct_frames_with_face() const { return pctFramesWithFace_; }
    [[nodiscard]] std::optional<double> mean_gaze_dx() const { return meanGazeDx_; }
    [[nodiscard]] std::optional<double> mean_gaze_dy() const { return meanGazeDy_; }
    [[nodiscard]] std::optional<double> pct_on_target() const { return pctOnTarget_; }

    /// Binary search by timestamp (frames() is written in chronological
    /// order by run_gaze2d.py, and defensively re-sorted here) with a
    /// before/after numerically-closer tie-break — mirrors
    /// GazeFusionResult::nearest_frame()'s exact convention. Returns
    /// nullptr only if frames() is empty.
    [[nodiscard]] const Gaze2dFrame* nearest_frame(int64_t timestampMsEstimate) const;

   private:
    bool valid_ = false;
    QString sourceVideo_;
    double minConfidence_ = 0.0;
    QVector<Gaze2dFrame> frames_;

    double pctFramesWithFace_ = 0.0;
    std::optional<double> meanGazeDx_;
    std::optional<double> meanGazeDy_;
    std::optional<double> pctOnTarget_;
};

} // namespace mosaic
