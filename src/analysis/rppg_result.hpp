#pragma once
#include <QRect>
#include <QString>
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
};

} // namespace mosaic
