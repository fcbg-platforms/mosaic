// Choosing the lens distortion model by its error on views left out of the
// fit (calibration/lens_model.hpp), on corners projected through a known lens.

#include <gtest/gtest.h>

#include <opencv2/calib3d.hpp>
#include <random>

#include "calibration/lens_model.hpp"

using mosaic::choose_lens_model;
using mosaic::kDefaultLensModel;
using mosaic::lens_models;

namespace {

const cv::Size kImage(1920, 1080);

struct Views {
    std::vector<std::vector<cv::Point3f>> obj;
    std::vector<std::vector<cv::Point2f>> img;
};

/// A 9 x 6 inner-corner board (40 mm squares) shown @p n times across the
/// picture, through a lens with square pixels and the given distortion,
/// with @p noisePx of corner noise; @p spread scales how far towards the
/// picture's edges the views go.
Views views_through(const cv::Mat& dist, int n, double noisePx, double spread = 1.0) {
    const cv::Matx33d K(1400, 0, 955, 0, 1400, 545, 0, 0, 1);
    std::vector<cv::Point3f> board;
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 9; ++c) {
            board.emplace_back(c * 40.0f - 160.0f, r * 40.0f - 100.0f, 0.0f);
        }
    }
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, noisePx);
    Views v;
    for (int i = 0; i < n; ++i) {
        // Across a 4 x 3 grid of places, tilted a different way each time.
        const double x = spread * (-650.0 + 1300.0 * (i % 4) / 3.0);
        const double y = spread * (-330.0 + 660.0 * ((i / 4) % 3) / 2.0);
        const cv::Vec3d rvec(0.35 * std::sin(i * 1.3), 0.35 * std::cos(i * 0.9), 0.2 * (i % 3));
        const cv::Vec3d tvec(x, y, 1500.0 + 150.0 * (i % 2));
        std::vector<cv::Point2f> px;
        cv::projectPoints(board, rvec, tvec, K, dist, px);
        bool inside = true;
        for (auto& p : px) {
            p.x += static_cast<float>(noise(rng));
            p.y += static_cast<float>(noise(rng));
            inside &= p.x > 0 && p.y > 0 && p.x < kImage.width && p.y < kImage.height;
        }
        if (inside) {
            v.obj.push_back(board);
            v.img.push_back(px);
        }
    }
    return v;
}

} // namespace

TEST(LensModel, ASimpleLensGetsTheSimplestModel) {
    // No k3 and square pixels: the extra freedom only fits noise.
    const cv::Mat dist = (cv::Mat_<double>(5, 1) << -0.12, 0.08, 0.0, 0.0, 0.0);
    const Views v      = views_through(dist, 20, 0.25);
    ASSERT_GE(v.img.size(), 12u);
    const auto choice = choose_lens_model(v.obj, v.img, kImage);
    ASSERT_EQ(choice.holdoutRms.size(), lens_models().size());
    for (double r : choice.holdoutRms) {
        EXPECT_GT(r, 0.0);
        EXPECT_LT(r, 0.6);
    }
    EXPECT_EQ(choice.chosen, 0) << mosaic::describe_lens_model_choice(choice);
}

TEST(LensModel, TooFewViewsKeepTheDefault) {
    const cv::Mat dist = (cv::Mat_<double>(5, 1) << -0.12, 0.08, 0.0, 0.0, 0.0);
    const Views all    = views_through(dist, 20, 0.25);
    const Views few{{all.obj.begin(), all.obj.begin() + 5}, {all.img.begin(), all.img.begin() + 5}};
    const auto choice = choose_lens_model(few.obj, few.img, kImage);
    EXPECT_EQ(choice.chosen, kDefaultLensModel);
    for (double r : choice.holdoutRms) {
        EXPECT_LT(r, 0.0);
    }
    EXPECT_NE(mosaic::describe_lens_model_choice(choice).find("not scored"), std::string::npos);
}

TEST(LensModel, ALensWithRealK3GetsTheK3Model) {
    // Strong distortion towards the picture's edges: k1 k2 alone cannot
    // describe it, which shows on the views left out of the fit.
    const cv::Mat dist = (cv::Mat_<double>(5, 1) << -0.30, 0.25, 0.0, 0.0, -0.60);
    const Views v      = views_through(dist, 24, 0.25, 1.25);
    ASSERT_GE(v.img.size(), 12u);
    const auto choice = choose_lens_model(v.obj, v.img, kImage);
    EXPECT_EQ(choice.chosen, 2) << mosaic::describe_lens_model_choice(choice);
}
