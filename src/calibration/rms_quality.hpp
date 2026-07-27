#pragma once

namespace mosaic {

enum class RmsQuality { Excellent, Good, Acceptable, Poor };

// Buckets a reprojection RMS error (px) using the thresholds already used by
// CalibrationW's tooltip text: <0.5 excellent, <1.0 good, <2.0 acceptable,
// else poor.
[[nodiscard]] RmsQuality rms_quality_for(double rmsErrorPx);

} // namespace mosaic
