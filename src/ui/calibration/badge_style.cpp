#include "ui/calibration/badge_style.hpp"

namespace mosaic {

namespace {

QString pill_stylesheet(const QString& fg, const QString& bgAlpha) {
    return QString(
               "QLabel { color: %1; background: %2; border: 1px solid %1; "
               "border-radius: 4px; padding: 1px 8px; font-weight: 600; }")
        .arg(fg, bgAlpha);
}

} // namespace

QString badge_stylesheet(RmsQuality quality) {
    switch (quality) {
        case RmsQuality::Excellent:
        case RmsQuality::Good:
            return pill_stylesheet("#44cc88", "rgba(68, 204, 136, 0.15)");
        case RmsQuality::Acceptable:
            return pill_stylesheet("#ddaa44", "rgba(221, 170, 68, 0.15)");
        case RmsQuality::Poor:
            return pill_stylesheet("#cc4444", "rgba(204, 68, 68, 0.15)");
    }
    return pill_stylesheet("#cc4444", "rgba(204, 68, 68, 0.15)");
}

QString badge_stylesheet(bool goodState) {
    return goodState ? pill_stylesheet("#44cc88", "rgba(68, 204, 136, 0.15)")
                     : pill_stylesheet("#cc4444", "rgba(204, 68, 68, 0.15)");
}

} // namespace mosaic
