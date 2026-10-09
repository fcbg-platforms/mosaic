#pragma once
#include <QRect>
#include <QString>
#include <QVector>
#include <cstdint>
#include <optional>

namespace mosaic {

/// One video frame of analysis/run_face_dynamics.py's "frames" array. When
/// faceDetected is false every other measurement is meaningless. A
/// measurement the script wrote as null (a head-pose fit that failed, the
/// first frame's head speed) is NaN.
struct FaceDynamicsFrame {
    int frameIndex      = 0;
    int64_t timestampMs = 0;
    bool faceDetected   = false;
    QRect faceBoxPx;
    double opennessLeft  = 0.0; ///< Eye opening relative to the open-eye baseline, ~1 open.
    double opennessRight = 0.0;
    double smile         = 0.0; ///< mouthSmile blendshape, 0..1.
    double browRaise     = 0.0; ///< Brow-raise blendshapes, 0..1.
    double expressivity  = 0.0; ///< Mean expressive blendshape above the person's rest.
    double yaw           = 0.0; ///< Head angles relative to the camera, degrees.
    double pitch         = 0.0;
    double roll          = 0.0;
    double headSpeed     = 0.0; ///< Head rotation speed, degrees per second.
};

/// One behaviour of the "events" array: a blink, smile, nod and so on.
struct FaceDynamicsEvent {
    QString kind; ///< blink, long_closure, smile, duchenne_smile, brow_raise, nod, shake.
    int64_t startMs   = 0;
    int64_t endMs     = 0;
    int64_t peakMs    = 0;
    double durationMs = 0.0;
    double peak       = 0.0; ///< Strength at the peak; meaning depends on kind.
};

/// The "summary" object: per-recording numbers. A value the script wrote as
/// null (nothing to average) is std::nullopt.
struct FaceDynamicsSummary {
    double faceSeenS   = 0.0;
    double faceSeenPct = 0.0;
    int blinks         = 0;
    std::optional<double> blinksPerMinute;
    std::optional<double> medianBlinkMs;
    std::optional<double> meanBlinkIntervalS;
    int longClosures = 0;
    std::optional<double> perclosPct;
    int smiles         = 0;
    int duchenneSmiles = 0;
    std::optional<double> smilingPct;
    int browRaises  = 0;
    int browFlashes = 0;
    std::optional<double> expressivityMean;
    int nods   = 0;
    int shakes = 0;
    std::optional<double> meanHeadSpeed;
};

/// Parses a "<video_stem>.face_dynamics.json" file written by
/// analysis/run_face_dynamics.py (schema mosaic-face-dynamics-v1), for the
/// Analysis tab's Face Dynamics plugin.
class FaceDynamicsResult {
   public:
    FaceDynamicsResult() = default;

    /// Parses jsonPath. Returns an invalid result if the file is missing,
    /// malformed or of another schema.
    static FaceDynamicsResult load(const QString& jsonPath);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] const QString& source_video() const { return sourceVideo_; }
    [[nodiscard]] int camera_index() const { return cameraIndex_; }
    [[nodiscard]] double fps() const { return fps_; }
    /// Session-relative path of the annotated video, empty when none was written.
    [[nodiscard]] const QString& annotated_video() const { return annotatedVideo_; }
    [[nodiscard]] const QVector<FaceDynamicsFrame>& frames() const { return frames_; }
    [[nodiscard]] const QVector<FaceDynamicsEvent>& events() const { return events_; }
    [[nodiscard]] const FaceDynamicsSummary& summary() const { return summary_; }

    /// The frame closest in time (nullptr only when there are no frames).
    [[nodiscard]] const FaceDynamicsFrame* nearest_frame(int64_t timestampMs) const;

   private:
    bool valid_ = false;
    QString sourceVideo_;
    int cameraIndex_ = 0;
    double fps_      = 0.0;
    QString annotatedVideo_;
    QVector<FaceDynamicsFrame> frames_;
    QVector<FaceDynamicsEvent> events_;
    FaceDynamicsSummary summary_;
};

} // namespace mosaic
