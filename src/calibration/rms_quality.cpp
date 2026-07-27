#include "calibration/rms_quality.hpp"

namespace mosaic {

RmsQuality rms_quality_for(double rmsErrorPx) {
    if (rmsErrorPx < 0.5) { return RmsQuality::Excellent; }
    if (rmsErrorPx < 1.0) { return RmsQuality::Good; }
    if (rmsErrorPx < 2.0) { return RmsQuality::Acceptable; }
    return RmsQuality::Poor;
}

} // namespace mosaic
