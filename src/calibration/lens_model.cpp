#include "calibration/lens_model.hpp"

#if defined(MOSAIC_HAVE_OPENCV)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <opencv2/calib3d.hpp>

namespace mosaic {

const std::vector<LensModel>& lens_models() {
    // k4..k6 are never used: the rest of MOSAIC reads five terms.
    static const std::vector<LensModel> kModels = {
        {"k1 k2, square pixels",
         cv::CALIB_FIX_K3 | cv::CALIB_FIX_K4 | cv::CALIB_FIX_K5 | cv::CALIB_FIX_ASPECT_RATIO},
        {"k1 k2", cv::CALIB_FIX_K3 | cv::CALIB_FIX_K4 | cv::CALIB_FIX_K5},
        {"k1 k2 k3", cv::CALIB_FIX_K4 | cv::CALIB_FIX_K5},
    };
    return kModels;
}

namespace {

// Fitted on `train`, the squared reprojection errors summed over every
// corner of the `test` views (each view's pose solved with that lens), and
// the number of corners. False when the fit fails.
bool score_fold(const std::vector<std::vector<cv::Point3f>>& obj,
                const std::vector<std::vector<cv::Point2f>>& img, cv::Size size, int flags,
                const std::vector<size_t>& train, const std::vector<size_t>& test, double& sumSq,
                size_t& count) {
    std::vector<std::vector<cv::Point3f>> trObj;
    std::vector<std::vector<cv::Point2f>> trImg;
    for (size_t i : train) {
        trObj.push_back(obj[i]);
        trImg.push_back(img[i]);
    }
    // FIX_ASPECT_RATIO keeps fx/fy as in the starting matrix: 1.
    cv::Mat K    = cv::Mat::eye(3, 3, CV_64F);
    cv::Mat dist = cv::Mat::zeros(5, 1, CV_64F);
    std::vector<cv::Mat> rvecs, tvecs;
    try {
        cv::calibrateCamera(trObj, trImg, size, K, dist, rvecs, tvecs, flags);
        for (size_t i : test) {
            cv::Mat rvec, tvec;
            if (!cv::solvePnP(obj[i], img[i], K, dist, rvec, tvec)) {
                return false;
            }
            std::vector<cv::Point2f> proj;
            cv::projectPoints(obj[i], rvec, tvec, K, dist, proj);
            for (size_t j = 0; j < proj.size(); ++j) {
                const cv::Point2f d = proj[j] - img[i][j];
                sumSq += static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y;
                ++count;
            }
        }
    } catch (const cv::Exception&) {
        return false;
    }
    return true;
}

} // namespace

LensModelChoice choose_lens_model(const std::vector<std::vector<cv::Point3f>>& objectPoints,
                                  const std::vector<std::vector<cv::Point2f>>& imagePoints,
                                  cv::Size imageSize) {
    const auto& models = lens_models();
    LensModelChoice out;
    out.holdoutRms.assign(models.size(), -1.0);
    const size_t n = imagePoints.size();
    if (n < static_cast<size_t>(kMinViewsToChooseLensModel) || objectPoints.size() != n) {
        return out;
    }

    const size_t folds = std::min<size_t>(5, n);
    for (size_t m = 0; m < models.size(); ++m) {
        double sumSq = 0.0;
        size_t count = 0;
        bool ok      = true;
        for (size_t f = 0; f < folds && ok; ++f) {
            std::vector<size_t> train, test;
            for (size_t i = 0; i < n; ++i) {
                (i % folds == f ? test : train).push_back(i);
            }
            ok = score_fold(objectPoints, imagePoints, imageSize, models[m].flags, train, test,
                            sumSq, count);
        }
        if (ok && count > 0) {
            out.holdoutRms[m] = std::sqrt(sumSq / static_cast<double>(count));
        }
    }

    double best = -1.0;
    for (double r : out.holdoutRms) {
        if (r >= 0.0 && (best < 0.0 || r < best)) {
            best = r;
        }
    }
    if (best < 0.0) {
        return out; // nothing could be scored: the default
    }
    // Held-out errors a few hundredths of a pixel apart differ by noise, not
    // by merit: prefer the simpler model then.
    out.tolerance = std::max(0.05, 0.10 * best);
    for (size_t m = 0; m < models.size(); ++m) {
        if (out.holdoutRms[m] >= 0.0 && out.holdoutRms[m] <= best + out.tolerance) {
            out.chosen = static_cast<int>(m);
            break;
        }
    }
    return out;
}

std::string describe_lens_model_choice(const LensModelChoice& choice) {
    const auto& models = lens_models();
    std::string s;
    for (size_t m = 0; m < models.size() && m < choice.holdoutRms.size(); ++m) {
        if (!s.empty()) {
            s += "; ";
        }
        s += models[m].name;
        if (choice.holdoutRms[m] >= 0.0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " %.3f px", choice.holdoutRms[m]);
            s += buf;
        } else {
            s += " not scored";
        }
        if (static_cast<int>(m) == choice.chosen) {
            s += " (chosen)";
        }
    }
    return s;
}

} // namespace mosaic

#endif
