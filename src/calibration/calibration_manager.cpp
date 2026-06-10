#include "calibration/calibration_manager.hpp"
#include "utils/logger.hpp"
#include <QImage>
#include <QThread>
#include <vector>

#if defined(MOSAIC_HAVE_OPENCV)
#  include <opencv2/calib3d.hpp>
#  include <opencv2/imgproc.hpp>
#endif

namespace mosaic {

// ── Data ──────────────────────────────────────────────────────────────────

#if defined(MOSAIC_HAVE_OPENCV)
struct ObjectPoint  { std::vector<cv::Point3f> data; };
struct ImagePoints  { std::vector<std::vector<cv::Point2f>> data; };
#endif

struct CalibrationManager::Impl {
    BoardSpec      board;
    CalibrationData result;
    bool           hasResult{false};

#if defined(MOSAIC_HAVE_OPENCV)
    std::vector<std::vector<cv::Point3f>> objectPoints;
    std::vector<std::vector<cv::Point2f>> imagePoints;
    cv::Size imageSize{0, 0};
#endif
};

CalibrationManager::CalibrationManager(QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>()) {}

CalibrationManager::~CalibrationManager() = default;

bool CalibrationManager::is_available() {
#if defined(MOSAIC_HAVE_OPENCV)
    return true;
#else
    return false;
#endif
}

void CalibrationManager::set_board(const BoardSpec& spec) { d->board = spec; }

// ── Frame feeding ──────────────────────────────────────────────────────────

bool CalibrationManager::feed_frame(const VideoFrame& frame) {
    if (!frame.is_valid()) { return false; }

#if defined(MOSAIC_HAVE_OPENCV)
    // Wrap the BGR8 frame data into an OpenCV Mat (no copy).
    const cv::Mat bgr(frame.height, frame.width, CV_8UC3,
                      const_cast<uint8_t*>(frame.data.data()),
                      static_cast<size_t>(frame.stride));

    cv::Mat grey;
    cv::cvtColor(bgr, grey, cv::COLOR_BGR2GRAY);

    const cv::Size patternSize(d->board.cols, d->board.rows);
    std::vector<cv::Point2f> corners;
    const bool found = cv::findChessboardCorners(
        grey, patternSize, corners,
        cv::CALIB_CB_ADAPTIVE_THRESH | cv::CALIB_CB_NORMALIZE_IMAGE | cv::CALIB_CB_FAST_CHECK);

    const int viewIdx = static_cast<int>(d->imagePoints.size());

    // Build preview image regardless of detection result.
    cv::Mat preview;
    cv::cvtColor(bgr, preview, cv::COLOR_BGR2RGB);
    if (found) {
        cv::cornerSubPix(grey, corners, cv::Size(11, 11), cv::Size(-1, -1),
                         cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 30, 0.001));
        cv::drawChessboardCorners(preview, patternSize, corners, found);
    }

    const QImage previewImg(preview.data, preview.cols, preview.rows,
                             static_cast<int>(preview.step),
                             QImage::Format_RGB888);

    if (found) {
        // Build 3-D object points for this view.
        std::vector<cv::Point3f> objPts;
        objPts.reserve(static_cast<size_t>(d->board.rows * d->board.cols));
        for (int row = 0; row < d->board.rows; ++row) {
            for (int col = 0; col < d->board.cols; ++col) {
                objPts.emplace_back(
                    static_cast<float>(col)  * static_cast<float>(d->board.squareSizeMm),
                    static_cast<float>(row)  * static_cast<float>(d->board.squareSizeMm),
                    0.0f);
            }
        }
        d->objectPoints.push_back(std::move(objPts));
        d->imagePoints.push_back(std::move(corners));
        d->imageSize = grey.size();
    }

    emit corners_detected(viewIdx, found, previewImg.copy());
    return found;
#else
    (void)frame;
    return false;
#endif
}

int  CalibrationManager::view_count()  const {
#if defined(MOSAIC_HAVE_OPENCV)
    return static_cast<int>(d->imagePoints.size());
#else
    return 0;
#endif
}

void CalibrationManager::clear_views() {
#if defined(MOSAIC_HAVE_OPENCV)
    d->objectPoints.clear();
    d->imagePoints.clear();
#endif
    d->hasResult = false;
}

// ── Calibration ────────────────────────────────────────────────────────────

void CalibrationManager::calibrate() {
#if defined(MOSAIC_HAVE_OPENCV)
    if (d->imagePoints.size() < 5) {
        log_warning("[Calibration] Need at least 5 views — calibration skipped.");
        emit calibration_done(-1.0, false);
        return;
    }

    emit calibration_started();

    // Run on a lambda thread so the main event loop is not blocked.
    auto* worker = QThread::create([this] {
        emit calibration_progress(5);

        cv::Mat cameraMatrix = cv::Mat::eye(3, 3, CV_64F);
        cv::Mat distCoeffs   = cv::Mat::zeros(5, 1, CV_64F);
        std::vector<cv::Mat> rvecs, tvecs;

        const double rms = cv::calibrateCamera(
            d->objectPoints, d->imagePoints, d->imageSize,
            cameraMatrix, distCoeffs, rvecs, tvecs,
            cv::CALIB_FIX_K4 | cv::CALIB_FIX_K5);

        emit calibration_progress(90);

        CalibrationData result;
        result.calibrated = true;
        result.rmsError   = rms;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                result.cameraMatrix[static_cast<size_t>(row * 3 + col)] =
                    cameraMatrix.at<double>(row, col);
            }
        }
        for (int idx = 0; idx < 5; ++idx) {
            result.distCoeffs[static_cast<size_t>(idx)] = distCoeffs.at<double>(idx);
        }
        d->result    = result;
        d->hasResult = true;

        emit calibration_progress(100);
        emit calibration_done(rms, true);

        log_info(QString("[Calibration] Done. RMS reprojection error: %1 px")
                     .arg(rms, 0, 'f', 3));
    });

    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
#else
    log_warning("[Calibration] OpenCV not available — calibration disabled.");
    emit calibration_done(-1.0, false);
#endif
}

CalibrationData CalibrationManager::result()     const { return d->result; }
bool            CalibrationManager::has_result() const { return d->hasResult; }

// ── Stereo calibration ─────────────────────────────────────────────────────

bool CalibrationManager::stereo_calibrate(CalibrationData& cam0,
                                           CalibrationData& cam1,
                                           const BoardSpec& spec) {
#if defined(MOSAIC_HAVE_OPENCV)
    if (!cam0.calibrated || !cam1.calibrated) {
        log_error("[Calibration] Both cameras must be individually calibrated before stereo.");
        return false;
    }

    // Build the camera matrices from the stored arrays.
    auto make_mat = [](const std::array<double, 9>& arr) {
        cv::Mat mat(3, 3, CV_64F);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                mat.at<double>(row, col) = arr[static_cast<size_t>(row * 3 + col)];
            }
        }
        return mat;
    };

    cv::Mat K0 = make_mat(cam0.cameraMatrix);
    cv::Mat K1 = make_mat(cam1.cameraMatrix);
    cv::Mat D0 = cv::Mat(5, 1, CV_64F, cam0.distCoeffs.data());
    cv::Mat D1 = cv::Mat(5, 1, CV_64F, cam1.distCoeffs.data());

    // We need shared object/image points from a simultaneous capture.
    // This API is simplified: the caller must supply them externally in a future
    // extension. For now log an informative message.
    log_warning("[Calibration] Stereo calibration requires simultaneous capture support "
                "(planned feature). Single-camera calibrations are stored.");
    (void)spec;
    return false;
#else
    (void)cam0; (void)cam1; (void)spec;
    return false;
#endif
}

} // namespace mosaic
