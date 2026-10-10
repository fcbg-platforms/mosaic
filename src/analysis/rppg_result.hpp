#pragma once
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>
#include <limits>
#include <optional>

namespace mosaic {

/// One HR-analysis window's estimate. Mirrors run_rppg.py's "windows" JSON
/// array exactly. bpm/smoothedBpm/snrDb are NaN when this window had no
/// reliable estimate ("no_face"/insufficient-data — never fabricated, see
/// run_rppg.py's MIN_VALID_FRACTION gate).
struct RppgWindow {
    int64_t startMs           = 0;
    int64_t endMs             = 0;
    double bpm                = std::numeric_limits<double>::quiet_NaN();
    double smoothedBpm        = std::numeric_limits<double>::quiet_NaN();
    double snrDb              = std::numeric_limits<double>::quiet_NaN();
    double validFrameFraction = 0.0;
};

/// One processed video frame's face-ROI detection, for the debug overlay
/// (PoseOverlayPlayerW::set_rppg_result()) — a separate, finer granularity
/// from RppgWindow's HR-analysis windows. faceDetected == false means
/// roiBboxPx is meaningless (default-constructed, not a real box).
struct RppgFrame {
    int frameIndex      = 0;
    int64_t timestampMs = 0;
    bool faceDetected   = false;
    QRect roiBboxPx;
};

/// One beat-to-beat interval (schema v2's "intervals"), at the second
/// beat's time. nn is false for an artifact (implausible, a sudden jump, or
/// next to a poorly shaped beat).
struct RppgInterval {
    int64_t tMs  = 0;
    double ibiMs = 0.0;
    bool nn      = false;
};

/// Heart rate and RMSSD over a sliding window of NN intervals (schema v2's
/// "hrv_windows"); NaN where the window had too few intervals.
struct RppgHrvWindow {
    int64_t startMs = 0;
    int64_t endMs   = 0;
    double hrBpm    = std::numeric_limits<double>::quiet_NaN();
    double rmssdMs  = std::numeric_limits<double>::quiet_NaN();
};

/// Heart-rate variability of the whole recording (schema v2's "hrv").
/// Values the script left out or wrote as null are std::nullopt.
struct RppgHrv {
    int nnCount = 0;
    std::optional<double> meanHrBpm;
    std::optional<double> sdnnMs;
    std::optional<double> rmssdMs;
    std::optional<double> pnn50Pct;
    std::optional<double> lfHf;
    std::optional<double> timingJitterMs;   ///< Beat-timing noise from the two face halves.
    std::optional<double> rmssdCorrectedMs; ///< RMSSD with that noise taken out.
    std::optional<double> sdnnCorrectedMs;
};

/// Parses a "<video_stem>.<backend>.rppg.json" file written by
/// analysis/run_rppg.py into a queryable in-memory structure, for the
/// Analysis tab's Remote Heart Rate (rPPG) plugin.
///
/// @warning EXPERIMENTAL — this is a research-grade heart-rate estimate
/// only, not a medical device and not clinically validated (see the
/// plugin's own persistent UI disclaimer banner).
///
/// Usage:
/// @code
///   auto result = RppgResult::load(jsonPath);
///   if (result.is_valid()) { ... }
/// @endcode
class RppgResult {
   public:
    RppgResult() = default;

    /// Parses jsonPath. Returns a default-constructed (is_valid() == false)
    /// result if the file is missing or malformed.
    static RppgResult load(const QString& jsonPath);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] const QString& source_video() const { return sourceVideo_; }
    [[nodiscard]] const QString& backend() const { return backend_; }
    [[nodiscard]] double window_sec() const { return windowSec_; }
    [[nodiscard]] double hop_sec() const { return hopSec_; }
    [[nodiscard]] const QVector<RppgWindow>& windows() const { return windows_; }
    [[nodiscard]] const QVector<RppgFrame>& frames() const { return frames_; }

    /// Precomputed by run_rppg.py from the raw (non-smoothed) per-window
    /// bpm values — not recomputed here, avoiding duplicating percentile/
    /// mean math in two languages. std::nullopt if no window in the file
    /// had any reliable estimate.
    [[nodiscard]] std::optional<double> mean_bpm() const { return meanBpm_; }
    [[nodiscard]] std::optional<double> median_bpm() const { return medianBpm_; }
    [[nodiscard]] std::optional<double> min_bpm() const { return minBpm_; }
    [[nodiscard]] std::optional<double> max_bpm() const { return maxBpm_; }
    [[nodiscard]] double pct_windows_good() const { return pctWindowsGood_; }

    /// Schema v2 (beats and HRV); empty or nullopt for a v1 file.
    [[nodiscard]] const QVector<RppgInterval>& intervals() const { return intervals_; }
    [[nodiscard]] const QVector<RppgHrvWindow>& hrv_windows() const { return hrvWindows_; }
    /// The recording's HRV, or nullopt when it was withheld (see
    /// hrv_withheld() for why) or the file predates it.
    [[nodiscard]] const std::optional<RppgHrv>& hrv() const { return hrv_; }
    [[nodiscard]] const QStringList& hrv_withheld() const { return hrvWithheld_; }
    /// Mean heart rate while the person on camera speaks / listens (needs
    /// Conversation Timing's output); nullopt without it.
    [[nodiscard]] std::optional<double> hr_speaking() const { return hrSpeaking_; }
    [[nodiscard]] std::optional<double> hr_listening() const { return hrListening_; }

    /// Binary search by start time (windows()/frames() are both written in
    /// chronological order by run_rppg.py) with a before/after
    /// numerically-closer tie-break — mirrors GazeFusionResult::
    /// nearest_frame()'s exact convention. Returns nullptr only if
    /// windows()/frames() is empty.
    [[nodiscard]] const RppgWindow* nearest_window(int64_t timestampMsEstimate) const;
    [[nodiscard]] const RppgFrame* nearest_frame(int64_t timestampMsEstimate) const;

   private:
    bool valid_ = false;
    QString sourceVideo_;
    QString backend_;
    double windowSec_ = 0.0;
    double hopSec_    = 0.0;
    QVector<RppgWindow> windows_;
    QVector<RppgFrame> frames_;

    std::optional<double> meanBpm_;
    std::optional<double> medianBpm_;
    std::optional<double> minBpm_;
    std::optional<double> maxBpm_;
    double pctWindowsGood_ = 0.0;
    QVector<RppgInterval> intervals_;
    QVector<RppgHrvWindow> hrvWindows_;
    std::optional<RppgHrv> hrv_;
    QStringList hrvWithheld_;
    std::optional<double> hrSpeaking_;
    std::optional<double> hrListening_;
};

} // namespace mosaic
