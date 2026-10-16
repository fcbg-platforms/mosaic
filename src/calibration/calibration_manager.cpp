#include "calibration/calibration_manager.hpp"

#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <vector>

#include "utils/logger.hpp"

#if defined(MOSAIC_HAVE_OPENCV)
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include "calibration/marked_chessboard.hpp"
#endif

namespace mosaic {

namespace {
// A view whose board corners all lie within this share of the image diagonal
// of an accepted view's, with the board's outline the same shape (its sides
// and diagonals within this share of their length), adds nothing: the board
// was neither moved nor tilted.
constexpr double kSameViewFraction  = 0.03;
constexpr double kSameShapeFraction = 0.05;
// The image is split into this grid to tell how much of it the board's
// corners have covered across the accepted views.
constexpr int kCoverageCols = 8;
constexpr int kCoverageRows = 6;
} // namespace

// ── Data ──────────────────────────────────────────────────────────────────

struct CalibrationManager::Impl {
    BoardSpec board;
    CalibrationData result;
    bool hasResult{false};
    bool busy{false};
    QPointer<QThread> worker; // the running calibration, if any
    int usedViews{0};
    int droppedViews{0};

#if defined(MOSAIC_HAVE_OPENCV)
    std::vector<std::vector<cv::Point3f>> objectPoints;
    std::vector<std::vector<cv::Point2f>> imagePoints;
    cv::Size imageSize{0, 0};

    // The board's four outer inner corners: the same pose seen twice gives
    // the same four points, in whatever order the detector numbered them.
    std::array<cv::Point2f, 4> outline(const std::vector<cv::Point2f>& c) const {
        const int cols = board.cols;
        const int n    = static_cast<int>(c.size());
        return {c[0], c[static_cast<size_t>(cols - 1)], c[static_cast<size_t>(n - cols)],
                c[static_cast<size_t>(n - 1)]};
    }

    // The outline's six side and diagonal lengths, sorted (the order the
    // detector numbered the corners in does not matter).
    static std::array<double, 6> shape(const std::array<cv::Point2f, 4>& o) {
        std::array<double, 6> len{};
        size_t k = 0;
        for (size_t i = 0; i < 4; ++i) {
            for (size_t j = i + 1; j < 4; ++j) {
                len[k++] = cv::norm(o[i] - o[j]);
            }
        }
        std::sort(len.begin(), len.end());
        return len;
    }

    bool same_as_accepted(const std::vector<cv::Point2f>& corners, cv::Size size) const {
        const double tol = kSameViewFraction * std::hypot(size.width, size.height);
        const auto a     = outline(corners);
        const auto sa    = shape(a);
        for (const auto& view : imagePoints) {
            const auto b  = outline(view);
            const auto sb = shape(b);
            // Tilting the board in place barely moves its corners but
            // changes its outline's shape: a new view.
            bool sameShape = true;
            for (size_t k = 0; k < sa.size(); ++k) {
                if (std::abs(sa[k] - sb[k]) > kSameShapeFraction * std::max(sa[k], sb[k])) {
                    sameShape = false;
                    break;
                }
            }
            if (!sameShape) {
                continue;
            }
            double sum = 0.0;
            for (const auto& p : a) {
                double best = 1e18;
                for (const auto& q : b) {
                    best = std::min(best, static_cast<double>(cv::norm(p - q)));
                }
                sum += best;
            }
            if (sum / 4.0 < tol) {
                return true;
            }
        }
        return false;
    }
#endif
};

CalibrationManager::CalibrationManager(QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>()) {}

CalibrationManager::~CalibrationManager() {
    // The calibration thread posts its result to this object: let it finish
    // first (closing the app during a calibration).
    if (d->worker && d->worker->isRunning()) {
        d->worker->wait();
    }
}

bool CalibrationManager::is_available() {
#if defined(MOSAIC_HAVE_OPENCV)
    return true;
#else
    return false;
#endif
}

int CalibrationManager::set_board(const BoardSpec& spec) {
    if (spec == d->board) {
        return 0;
    }
    d->board            = spec;
    const int discarded = view_count();
#if defined(MOSAIC_HAVE_OPENCV)
    d->objectPoints.clear();
    d->imagePoints.clear();
    d->imageSize = cv::Size(0, 0);
#endif
    return discarded;
}

// ── Frame feeding ──────────────────────────────────────────────────────────

bool CalibrationManager::feed_frame(const VideoFrame& frame) {
    if (!frame.is_valid()) {
        return false;
    }

#if defined(MOSAIC_HAVE_OPENCV)
    // Wrap the BGR8 frame data into an OpenCV Mat (no copy).
    const cv::Mat bgr(frame.height, frame.width, CV_8UC3, const_cast<uint8_t*>(frame.data.data()),
                      static_cast<size_t>(frame.stride));

    cv::Mat grey;
    cv::cvtColor(bgr, grey, cv::COLOR_BGR2GRAY);

    const cv::Size patternSize(d->board.cols, d->board.rows);
    std::vector<cv::Point2f> corners = find_chessboard_corners(grey, patternSize);
    const bool found = corners.size() == static_cast<size_t>(d->board.cols * d->board.rows);

    // Build preview image regardless of detection result.
    cv::Mat preview;
    cv::cvtColor(bgr, preview, cv::COLOR_BGR2RGB);
    if (found) {
        cv::drawChessboardCorners(preview, patternSize, corners, found);
    }
    const QImage previewImg(preview.data, preview.cols, preview.rows,
                            static_cast<int>(preview.step), QImage::Format_RGB888);

    QString note;
    bool accepted = false;
    if (!found) {
        note = "No board found: the whole board must be in view.";
    } else if (!d->imagePoints.empty() && grey.size() != d->imageSize) {
        note = QString("Not used: this frame is %1x%2, the views so far are %3x%4.")
                   .arg(grey.cols)
                   .arg(grey.rows)
                   .arg(d->imageSize.width)
                   .arg(d->imageSize.height);
    } else if (d->same_as_accepted(corners, grey.size())) {
        note = "Not used: the board has not moved since an accepted view. Move or tilt it.";
    } else {
        std::vector<cv::Point3f> objPts;
        objPts.reserve(static_cast<size_t>(d->board.rows * d->board.cols));
        for (int row = 0; row < d->board.rows; ++row) {
            for (int col = 0; col < d->board.cols; ++col) {
                objPts.emplace_back(
                    static_cast<float>(col) * static_cast<float>(d->board.squareSizeMm),
                    static_cast<float>(row) * static_cast<float>(d->board.squareSizeMm), 0.0f);
            }
        }
        d->objectPoints.push_back(std::move(objPts));
        d->imagePoints.push_back(std::move(corners));
        d->imageSize = grey.size();
        accepted     = true;
        note         = QString("View %1 accepted.").arg(d->imagePoints.size());
    }

    log_info(QString("[Calibration] feed_frame: pattern %1x%2 %3 in %4x%5 frame: %6")
                 .arg(d->board.cols)
                 .arg(d->board.rows)
                 .arg(found ? "found" : "not found")
                 .arg(frame.width)
                 .arg(frame.height)
                 .arg(note));
    emit corners_detected(static_cast<int>(d->imagePoints.size()), accepted, previewImg.copy(),
                          note);
    return accepted;
#else
    (void)frame;
    return false;
#endif
}

int CalibrationManager::view_count() const {
#if defined(MOSAIC_HAVE_OPENCV)
    return static_cast<int>(d->imagePoints.size());
#else
    return 0;
#endif
}

double CalibrationManager::coverage() const {
#if defined(MOSAIC_HAVE_OPENCV)
    if (d->imagePoints.empty() || d->imageSize.width <= 0 || d->imageSize.height <= 0) {
        return 0.0;
    }
    std::vector<bool> hit(static_cast<size_t>(kCoverageCols * kCoverageRows), false);
    for (const auto& view : d->imagePoints) {
        for (const auto& p : view) {
            const int cx = std::clamp(static_cast<int>(p.x / d->imageSize.width * kCoverageCols), 0,
                                      kCoverageCols - 1);
            const int cy = std::clamp(static_cast<int>(p.y / d->imageSize.height * kCoverageRows),
                                      0, kCoverageRows - 1);
            hit[static_cast<size_t>(cy * kCoverageCols + cx)] = true;
        }
    }
    return static_cast<double>(std::count(hit.begin(), hit.end(), true)) /
           static_cast<double>(hit.size());
#else
    return 0.0;
#endif
}

void CalibrationManager::clear_views() {
#if defined(MOSAIC_HAVE_OPENCV)
    d->objectPoints.clear();
    d->imagePoints.clear();
    d->imageSize = cv::Size(0, 0);
#endif
    d->hasResult = false;
}

// ── Calibration ────────────────────────────────────────────────────────────

void CalibrationManager::calibrate() {
#if defined(MOSAIC_HAVE_OPENCV)
    if (d->busy) {
        return;
    }
    if (d->imagePoints.size() < 5) {
        log_warning("[Calibration] Need at least 5 views — calibration skipped.");
        emit calibration_done(-1.0, false);
        return;
    }

    d->busy = true;
    emit calibration_started();

    // The worker gets its own copy of the views: frames keep arriving (and
    // being added) on this thread while it runs.
    auto objectPoints   = d->objectPoints;
    auto imagePoints    = d->imagePoints;
    const cv::Size size = d->imageSize;
    // The destructor waits for this thread, so `self` outlives it; a result
    // posted just before destruction is dropped with the object.
    CalibrationManager* self = this;

    auto* worker = QThread::create([self, objectPoints, imagePoints, size]() mutable {
        auto run2 = [&size](const std::vector<std::vector<cv::Point3f>>& obj,
                            const std::vector<std::vector<cv::Point2f>>& img, cv::Mat& K,
                            cv::Mat& dist, std::vector<double>& perView) {
            std::vector<cv::Mat> rvecs, tvecs;
            cv::Mat stdIn, stdEx, errs;
            const double rms =
                cv::calibrateCamera(obj, img, size, K, dist, rvecs, tvecs, stdIn, stdEx, errs,
                                    cv::CALIB_FIX_K4 | cv::CALIB_FIX_K5);
            perView.assign(errs.begin<double>(), errs.end<double>());
            return rms;
        };
        auto run = [&](cv::Mat& K, cv::Mat& dist, std::vector<double>& perView) {
            return run2(objectPoints, imagePoints, K, dist, perView);
        };
        cv::Mat K    = cv::Mat::eye(3, 3, CV_64F);
        cv::Mat dist = cv::Mat::zeros(5, 1, CV_64F);
        std::vector<double> perView;
        double rms = -1.0;
        try {
            rms = run(K, dist, perView);
        } catch (const std::exception& e) {
            // Views that cannot constrain the lens (all nearly the same, or
            // degenerate): report a failed calibration, never crash.
            log_warning(QString("[Calibration] Failed: %1").arg(QString::fromUtf8(e.what())));
            QMetaObject::invokeMethod(
                self,
                [self] {
                    self->d->busy = false;
                    emit self->calibration_done(-1.0, false);
                },
                Qt::QueuedConnection);
            return;
        }

        // Views that fit far worse than the rest (a blurred frame, a corner
        // found in the wrong place) pull the whole result: leave them out and
        // calibrate once more, as long as enough views remain.
        int dropped = 0;
        if (perView.size() >= 8) {
            std::vector<double> sorted = perView;
            std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(sorted.size() / 2),
                             sorted.end());
            const double limit = std::max(1.0, 3.0 * sorted[sorted.size() / 2]);
            std::vector<std::vector<cv::Point3f>> keepObj;
            std::vector<std::vector<cv::Point2f>> keepImg;
            for (size_t i = 0; i < perView.size(); ++i) {
                if (perView[i] <= limit) {
                    keepObj.push_back(objectPoints[i]);
                    keepImg.push_back(imagePoints[i]);
                }
            }
            if (keepObj.size() < perView.size() && keepObj.size() >= 6) {
                cv::Mat K2    = cv::Mat::eye(3, 3, CV_64F);
                cv::Mat dist2 = cv::Mat::zeros(5, 1, CV_64F);
                std::vector<double> perView2;
                try {
                    const double rms2 = run2(keepObj, keepImg, K2, dist2, perView2);
                    dropped           = static_cast<int>(perView.size() - keepObj.size());
                    objectPoints      = std::move(keepObj);
                    imagePoints       = std::move(keepImg);
                    K                 = K2;
                    dist              = dist2;
                    rms               = rms2;
                } catch (const std::exception&) {
                    // Keep the first result, from every view.
                }
            }
        }

        CalibrationData result;
        result.calibrated  = true;
        result.rmsError    = rms;
        result.imageWidth  = size.width;
        result.imageHeight = size.height;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                result.cameraMatrix[static_cast<size_t>(row * 3 + col)] = K.at<double>(row, col);
            }
        }
        for (int idx = 0; idx < 5; ++idx) {
            result.distCoeffs[static_cast<size_t>(idx)] = dist.at<double>(idx);
        }
        const int used = static_cast<int>(imagePoints.size());

        // Hand the result over on the manager's own thread.
        QMetaObject::invokeMethod(
            self,
            [self, result, rms, used, dropped] {
                self->d->result       = result;
                self->d->hasResult    = true;
                self->d->usedViews    = used;
                self->d->droppedViews = dropped;
                self->d->busy         = false;
                log_info(QString("[Calibration] Done. RMS reprojection error: %1 px from %2 "
                                 "view(s), %3 left out as outliers.")
                             .arg(rms, 0, 'f', 3)
                             .arg(used)
                             .arg(dropped));
                emit self->calibration_done(rms, true);
            },
            Qt::QueuedConnection);
    });

    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    d->worker = worker;
    worker->start();
#else
    log_warning("[Calibration] OpenCV not available — calibration disabled.");
    emit calibration_done(-1.0, false);
#endif
}

CalibrationData CalibrationManager::result() const { return d->result; }
bool CalibrationManager::has_result() const { return d->hasResult; }
int CalibrationManager::used_view_count() const { return d->usedViews; }
int CalibrationManager::dropped_view_count() const { return d->droppedViews; }

} // namespace mosaic
