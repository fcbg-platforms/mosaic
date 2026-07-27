#include "calibration/room_calibration_manager.hpp"
#include "utils/logger.hpp"
#include <cmath>
#include <map>
#include <utility>

#if defined(MOSAIC_HAVE_OPENCV)
#  include <opencv2/calib3d.hpp>
#  include <opencv2/imgproc.hpp>
#  include <opencv2/objdetect.hpp>
#endif

namespace mosaic {

using room_frame::Mat4;

namespace {

#if defined(MOSAIC_HAVE_OPENCV)

cv::Mat make_camera_matrix(const std::array<double, 9>& arr) {
    cv::Mat mat(3, 3, CV_64F);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            mat.at<double>(row, col) = arr[static_cast<size_t>(row * 3 + col)];
        }
    }
    return mat;
}

cv::Mat make_dist_coeffs(const std::array<double, 5>& arr) {
    cv::Mat mat(5, 1, CV_64F);
    for (int i = 0; i < 5; ++i) {
        mat.at<double>(i) = arr[static_cast<size_t>(i)];
    }
    return mat;
}

// rvec/tvec (as produced by cv::solvePnP) -> 4x4 row-major Mat4.
Mat4 rt_to_mat4(const cv::Mat& rvec, const cv::Mat& tvec) {
    cv::Mat rot;
    cv::Rodrigues(rvec, rot);
    Mat4 m = room_frame::identity();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            m[static_cast<size_t>(row * 4 + col)] = rot.at<double>(row, col);
        }
    }
    m[3]  = tvec.at<double>(0);
    m[7]  = tvec.at<double>(1);
    m[11] = tvec.at<double>(2);
    return m;
}

// Inverse of rt_to_mat4() — needed to reproject a Mat4 pose with OpenCV.
void mat4_to_rt(const Mat4& m, cv::Mat& rvec, cv::Mat& tvec) {
    cv::Mat rot(3, 3, CV_64F);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            rot.at<double>(row, col) = m[static_cast<size_t>(row * 4 + col)];
        }
    }
    cv::Rodrigues(rot, rvec);
    tvec = (cv::Mat_<double>(3, 1) << m[3], m[7], m[11]);
}

#endif

// One camera's accepted board detection within a single shot.
struct ShotCameraEntry {
    int  cameraIndex = -1;
    Mat4 boardToCam  = room_frame::identity();
#if defined(MOSAIC_HAVE_OPENCV)
    std::vector<cv::Point2f> corners2d;
    std::vector<cv::Point3f> corners3d;
#endif
};

// One simultaneous multi-camera capture.
struct Shot {
    std::vector<ShotCameraEntry> cameras;
};

} // namespace

// ── Impl ──────────────────────────────────────────────────────────────────

struct RoomCalibrationManager::Impl {
    BoardSpec                    board;
    std::map<int, CalibrationData> intrinsics;
    std::vector<Shot>            shots;

    std::vector<bool>   resolved;
    std::vector<Mat4>   extrinsicRt;
    std::vector<double> reprojectionRms;

#if defined(MOSAIC_HAVE_OPENCV)
    cv::Ptr<cv::aruco::CharucoBoard> charucoBoard;

    void ensure_board() {
        if (charucoBoard) { return; }
        const cv::aruco::Dictionary dict = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_100);
        charucoBoard = cv::makePtr<cv::aruco::CharucoBoard>(
            cv::Size(board.cols, board.rows),
            static_cast<float>(board.squareLengthMm),
            static_cast<float>(board.markerLengthMm),
            dict);
    }
#endif
};

RoomCalibrationManager::RoomCalibrationManager() : d(std::make_unique<Impl>()) {}
RoomCalibrationManager::~RoomCalibrationManager() = default;

bool RoomCalibrationManager::is_available() {
#if defined(MOSAIC_HAVE_OPENCV)
    return true;
#else
    return false;
#endif
}

void RoomCalibrationManager::set_board(const BoardSpec& spec) {
    d->board = spec;
#if defined(MOSAIC_HAVE_OPENCV)
    d->charucoBoard.release();   // rebuilt lazily on next feed_shot()
#endif
}

void RoomCalibrationManager::set_camera_intrinsics(int cameraIndex, const CalibrationData& data) {
    d->intrinsics[cameraIndex] = data;
}

QVector<RoomCalibrationManager::CameraShotResult>
RoomCalibrationManager::feed_shot(const QVector<VideoFrame>& frames) {
    QVector<CameraShotResult> results;

#if defined(MOSAIC_HAVE_OPENCV)
    d->ensure_board();
    cv::aruco::CharucoParameters charucoParams;
    cv::aruco::DetectorParameters detectorParams;
    cv::aruco::CharucoDetector detector(*d->charucoBoard, charucoParams, detectorParams);

    Shot shot;
    for (const VideoFrame& frame : frames) {
        CameraShotResult r;
        r.cameraIndex = frame.cameraIndex;

        if (!frame.is_valid()) {
            results.push_back(r);
            continue;
        }

        const auto intrinsicsIt = d->intrinsics.find(frame.cameraIndex);
        if (intrinsicsIt == d->intrinsics.end() || !intrinsicsIt->second.calibrated) {
            log_warning(QString("[RoomCalibration] Camera %1 has no intrinsic calibration — "
                                 "shot skipped for this camera.")
                            .arg(frame.cameraIndex));
            results.push_back(r);
            continue;
        }

        const cv::Mat bgr(frame.height, frame.width, CV_8UC3,
                           const_cast<uint8_t*>(frame.data.data()),
                           static_cast<size_t>(frame.stride));
        cv::Mat grey;
        cv::cvtColor(bgr, grey, cv::COLOR_BGR2GRAY);

        cv::Mat charucoCorners, charucoIds;
        std::vector<std::vector<cv::Point2f>> markerCorners;
        std::vector<int> markerIds;
        detector.detectBoard(grey, charucoCorners, charucoIds, markerCorners, markerIds);

        r.cornerCount = charucoCorners.rows;
        if (charucoCorners.rows < 4) {
            results.push_back(r);
            continue;
        }

        std::vector<cv::Point3f> objPoints;
        std::vector<cv::Point2f> imgPoints;
        d->charucoBoard->matchImagePoints(charucoCorners, charucoIds, objPoints, imgPoints);
        if (objPoints.size() < 4) {
            results.push_back(r);
            continue;
        }

        const CalibrationData& intr = intrinsicsIt->second;
        const cv::Mat K    = make_camera_matrix(intr.cameraMatrix);
        const cv::Mat dist = make_dist_coeffs(intr.distCoeffs);

        cv::Mat rvec, tvec;
        const bool solved = cv::solvePnP(objPoints, imgPoints, K, dist, rvec, tvec,
                                          false, cv::SOLVEPNP_ITERATIVE);
        if (!solved) {
            results.push_back(r);
            continue;
        }

        ShotCameraEntry entry;
        entry.cameraIndex = frame.cameraIndex;
        entry.boardToCam  = rt_to_mat4(rvec, tvec);
        entry.corners2d   = imgPoints;
        entry.corners3d   = objPoints;
        shot.cameras.push_back(std::move(entry));

        r.found = true;
        results.push_back(r);
    }

    if (!shot.cameras.empty()) {
        d->shots.push_back(std::move(shot));
    }
#else
    for (const VideoFrame& frame : frames) {
        CameraShotResult r;
        r.cameraIndex = frame.cameraIndex;
        results.push_back(r);
    }
#endif

    return results;
}

int RoomCalibrationManager::shot_count() const {
    return static_cast<int>(d->shots.size());
}

void RoomCalibrationManager::clear_shots() {
    d->shots.clear();
    d->resolved.clear();
    d->extrinsicRt.clear();
    d->reprojectionRms.clear();
}

RoomCalibrationManager::SolveResult RoomCalibrationManager::solve(int cameraCount,
                                                                    int referenceCameraIndex) {
    SolveResult out;

#if defined(MOSAIC_HAVE_OPENCV)
    // Build the shared-shot graph: one Edge per unordered camera pair that
    // co-appeared in at least one shot, carrying every such shot's
    // index-aligned board-pose pair.
    std::map<std::pair<int, int>, room_frame::Edge> edgeMap;
    for (const auto& shot : d->shots) {
        for (size_t i = 0; i < shot.cameras.size(); ++i) {
            for (size_t j = i + 1; j < shot.cameras.size(); ++j) {
                int  a = shot.cameras[i].cameraIndex;
                int  b = shot.cameras[j].cameraIndex;
                Mat4 poseA = shot.cameras[i].boardToCam;
                Mat4 poseB = shot.cameras[j].boardToCam;
                if (a > b) {
                    std::swap(a, b);
                    std::swap(poseA, poseB);
                }
                auto& edge = edgeMap[{a, b}];
                edge.camA = a;
                edge.camB = b;
                edge.boardToCamA.push_back(poseA);
                edge.boardToCamB.push_back(poseB);
            }
        }
    }

    std::vector<room_frame::Edge> edges;
    edges.reserve(edgeMap.size());
    for (auto& [key, edge] : edgeMap) {
        (void)key;
        edges.push_back(std::move(edge));
    }

    const auto result = room_frame::bfs_resolve(cameraCount, referenceCameraIndex, edges);
    d->resolved    = result.resolved;
    d->extrinsicRt = result.extrinsicRt;

    // Per-camera reprojection RMS: how well each camera's OWN solvePnP
    // solution (from every shot where it directly saw the board) fits its
    // own detected corners — independent of BFS chain length, so this
    // reflects detection/solve quality, not accumulated chain error.
    d->reprojectionRms.assign(static_cast<size_t>(cameraCount), -1.0);
    std::vector<double> sumSqErr(static_cast<size_t>(cameraCount), 0.0);
    std::vector<int>    countPts(static_cast<size_t>(cameraCount), 0);

    for (const auto& shot : d->shots) {
        for (const auto& cam : shot.cameras) {
            if (cam.cameraIndex < 0 || cam.cameraIndex >= cameraCount) { continue; }
            const auto intrIt = d->intrinsics.find(cam.cameraIndex);
            if (intrIt == d->intrinsics.end()) { continue; }

            const cv::Mat K    = make_camera_matrix(intrIt->second.cameraMatrix);
            const cv::Mat dist = make_dist_coeffs(intrIt->second.distCoeffs);
            cv::Mat rvec, tvec;
            mat4_to_rt(cam.boardToCam, rvec, tvec);

            std::vector<cv::Point2f> projected;
            cv::projectPoints(cam.corners3d, rvec, tvec, K, dist, projected);

            double sq = 0.0;
            for (size_t k = 0; k < projected.size(); ++k) {
                const double dx = projected[k].x - cam.corners2d[k].x;
                const double dy = projected[k].y - cam.corners2d[k].y;
                sq += dx * dx + dy * dy;
            }
            sumSqErr[static_cast<size_t>(cam.cameraIndex)] += sq;
            countPts[static_cast<size_t>(cam.cameraIndex)] += static_cast<int>(projected.size());
        }
    }
    for (int i = 0; i < cameraCount; ++i) {
        if (countPts[static_cast<size_t>(i)] > 0) {
            d->reprojectionRms[static_cast<size_t>(i)] =
                std::sqrt(sumSqErr[static_cast<size_t>(i)] / countPts[static_cast<size_t>(i)]);
        }
    }

    out.resolved          = d->resolved;
    out.reprojectionRmsPx = d->reprojectionRms;
#else
    (void)cameraCount;
    (void)referenceCameraIndex;
#endif

    return out;
}

bool RoomCalibrationManager::is_resolved(int cameraIndex) const {
    if (cameraIndex < 0 || cameraIndex >= static_cast<int>(d->resolved.size())) { return false; }
    return d->resolved[static_cast<size_t>(cameraIndex)];
}

std::array<double, 16> RoomCalibrationManager::extrinsic_for(int cameraIndex) const {
    if (cameraIndex < 0 || cameraIndex >= static_cast<int>(d->extrinsicRt.size())) {
        return room_frame::identity();
    }
    return d->extrinsicRt[static_cast<size_t>(cameraIndex)];
}

double RoomCalibrationManager::reprojection_rms_for(int cameraIndex) const {
    if (cameraIndex < 0 || cameraIndex >= static_cast<int>(d->reprojectionRms.size())) { return -1.0; }
    return d->reprojectionRms[static_cast<size_t>(cameraIndex)];
}

bool RoomCalibrationManager::use_shot_as_plane(int shotIndex, int cameraIndex,
                                                std::array<double, 3>& outPoint,
                                                std::array<double, 3>& outNormal) const {
    if (shotIndex < 0 || shotIndex >= static_cast<int>(d->shots.size())) { return false; }
    if (!is_resolved(cameraIndex)) { return false; }

    const Shot& shot = d->shots[static_cast<size_t>(shotIndex)];
    const ShotCameraEntry* entry = nullptr;
    for (const auto& cam : shot.cameras) {
        if (cam.cameraIndex == cameraIndex) {
            entry = &cam;
            break;
        }
    }
    if (!entry) { return false; }

    const Mat4 boardToRoom = room_frame::compose(extrinsic_for(cameraIndex), entry->boardToCam);

    outPoint  = {boardToRoom[3], boardToRoom[7], boardToRoom[11]};
    // The board's printed-face normal is its local Z axis (ChArUco object
    // points lie in the board's own Z=0 plane) — the 3rd column of the
    // rotation part.
    outNormal = {boardToRoom[2], boardToRoom[6], boardToRoom[10]};
    return true;
}

} // namespace mosaic
