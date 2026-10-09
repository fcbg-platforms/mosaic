#pragma once
#include <QString>
#include <QVector>
#include <cstdint>
#include <optional>

namespace mosaic {

/// One video frame of analysis/run_eye_contact.py's "frames" array. Angles
/// are NaN where no gaze was measured; contact is 1 (looking at the
/// partner), 0 (looking away) or -1 (unknown).
struct EyeContactFrame {
    int frameIndex      = 0;
    int64_t timestampMs = 0;
    bool faceDetected   = false;
    double gazeYaw      = 0.0; ///< Camera-frame degrees, positive towards image right.
    double gazePitch    = 0.0; ///< Degrees, positive up.
    double offsetDeg    = 0.0; ///< Angle from the partner's direction.
    int contact         = -1;
};

/// One look-away. direction is "up", "down", "left" or "right" (the
/// subject's own left and right).
struct EyeContactAversion {
    int64_t startMs  = 0;
    int64_t endMs    = 0;
    int durationMs   = 0;
    double offsetDeg = 0.0;
    QString direction;
};

/// The "summary" object. Values the script wrote as null are std::nullopt.
struct EyeContactSummary {
    std::optional<double> eyeContactPct;
    std::optional<double> listeningPct; ///< Needs Conversation Timing output.
    std::optional<double> speakingPct;
    int aversions = 0;
    std::optional<double> aversionsPerMin;
    std::optional<double> aversionMedianS;
    int up = 0, down = 0, left = 0, right = 0;
    int subjectTurns = 0;
    std::optional<double> turnStartAversionPct;
    std::optional<double> turnEndContactPct;
    std::optional<double> gapAversionPct;
};

/// Parses a "<video_stem>.eye_contact.json" written by
/// analysis/run_eye_contact.py (schema mosaic-eye-contact-v1), for the
/// Analysis tab's Eye Contact plugin.
class EyeContactResult {
   public:
    EyeContactResult() = default;

    /// Invalid when the file is missing, malformed or of another schema.
    static EyeContactResult load(const QString& jsonPath);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] const QString& source_video() const { return sourceVideo_; }
    [[nodiscard]] const QString& annotated_video() const { return annotatedVideo_; }
    /// "auto", "camera" or "manual".
    [[nodiscard]] const QString& target_method() const { return targetMethod_; }
    [[nodiscard]] std::optional<double> target_yaw() const { return targetYaw_; }
    [[nodiscard]] std::optional<double> target_pitch() const { return targetPitch_; }
    [[nodiscard]] std::optional<double> radius_deg() const { return radiusDeg_; }
    [[nodiscard]] const QVector<EyeContactFrame>& frames() const { return frames_; }
    [[nodiscard]] const QVector<EyeContactAversion>& aversions() const { return aversions_; }
    [[nodiscard]] const EyeContactSummary& summary() const { return summary_; }

   private:
    bool valid_ = false;
    QString sourceVideo_;
    QString annotatedVideo_;
    QString targetMethod_;
    std::optional<double> targetYaw_;
    std::optional<double> targetPitch_;
    std::optional<double> radiusDeg_;
    QVector<EyeContactFrame> frames_;
    QVector<EyeContactAversion> aversions_;
    EyeContactSummary summary_;
};

} // namespace mosaic
