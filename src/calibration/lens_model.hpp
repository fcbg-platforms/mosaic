#pragma once

#if defined(MOSAIC_HAVE_OPENCV)
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace mosaic {

// ── Choosing the lens distortion model ──────────────────────────────────────
//
// More distortion terms always fit the views they were fitted on better,
// even when the extra terms only fit noise, and then they bend the picture's
// edges, where few corners were seen, in ways the lens does not. So each
// candidate model is fitted on most of the views and scored on the views it
// did not see, and the simplest model within a small tolerance of the best
// held-out error is chosen. From the 4by4k pipeline
// (scripts/calib_intrinsics.py, MIT licence).

/// One candidate model: OpenCV calibrateCamera flags, simplest first.
struct LensModel {
    const char* name; ///< "k1 k2, square pixels", "k1 k2", "k1 k2 k3"
    int flags;
};

/// The candidates, simplest first. Every one keeps the five-term
/// (k1, k2, p1, p2, k3) layout the rest of MOSAIC reads; a simpler model
/// just holds k3 at zero, or fx equal to fy.
[[nodiscard]] const std::vector<LensModel>& lens_models();

/// Index of the model calibration used before the choice existed (k1 k2
/// k3), used when there are too few views to choose.
inline constexpr int kDefaultLensModel = 2;

/// How each candidate did on views it was not fitted on.
struct LensModelChoice {
    int chosen = kDefaultLensModel;
    /// Per model (lens_models() order): the RMS error, px, on held-out
    /// views; negative when not scored (too few views, or the fit failed).
    std::vector<double> holdoutRms;
    /// The tolerance the choice allowed, px.
    double tolerance = 0.0;
};

/// The fewest views that can be split into training and held-out views.
inline constexpr int kMinViewsToChooseLensModel = 8;

/// Scores every model by k-fold cross-validation (up to 5 folds; view i
/// held out in fold i mod k): fitted on the other views, each held-out
/// view's pose is solved with the fitted lens and its corners reprojected.
/// Chooses the simplest model whose held-out RMS is within max(0.05 px,
/// 10%) of the best. Fewer than kMinViewsToChooseLensModel views: the
/// default model, nothing scored.
[[nodiscard]] LensModelChoice choose_lens_model(
    const std::vector<std::vector<cv::Point3f>>& objectPoints,
    const std::vector<std::vector<cv::Point2f>>& imagePoints, cv::Size imageSize);

/// A one-line summary for a tooltip or log: each model's held-out error,
/// the chosen one marked.
[[nodiscard]] std::string describe_lens_model_choice(const LensModelChoice& choice);

} // namespace mosaic

#endif
