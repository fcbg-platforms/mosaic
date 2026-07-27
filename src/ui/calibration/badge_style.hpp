#pragma once
#include "calibration/rms_quality.hpp"
#include <QString>

namespace mosaic {

// Inline stylesheet for a small rounded-rect status pill, reusing
// style.hpp's documented palette (success green #44cc88, warning amber
// #ddaa44, danger red #cc4444) rather than inventing new colors —
// Excellent/Good map to green, Acceptable to amber, Poor to red. Meant to be
// applied dynamically via setStyleSheet() as the underlying state changes
// (matching CameraCardW::set_connected()'s idiom), not set once at
// construction like the app's role="..." QSS-property variants.
[[nodiscard]] QString badge_stylesheet(RmsQuality quality);

// Same idea for a plain good/bad boolean state (resolved/unresolved,
// found/not-found) — true maps to the same success green, false to the same
// danger red.
[[nodiscard]] QString badge_stylesheet(bool goodState);

} // namespace mosaic
