#include "calibration/room_calibration_manager.hpp"

#include <cmath>
#include <map>
#include <optional>
#include <utility>

#include "calibration/marked_chessboard.hpp"
#include "utils/logger.hpp"

#if defined(MOSAIC_HAVE_OPENCV)
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
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
    int cameraIndex = -1;
    Mat4 boardToCam = room_frame::identity();
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
    BoardSpec board;
    std::map<int, CalibrationData> intrinsics;
    std::vector<Shot> shots;

    std::vector<bool> resolved;
    std::vector<Mat4> extrinsicRt;
    std::vector<double> reprojectionRms;
    std::vector<double> agreementMm;

#if defined(MOSAIC_HAVE_OPENCV)
    cv::Ptr<cv::aruco::CharucoBoard> charucoBoard;

    void ensure_board() {
        if (charucoBoard) {
            return;
        }
        const cv::aruco::Dictionary dict =
            cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_100);
        charucoBoard = cv::makePtr<cv::aruco::CharucoBoard>(
            cv::Size(board.cols, board.rows), static_cast<float>(board.squareLengthMm),
            static_cast<float>(board.markerLengthMm), dict);
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

int RoomCalibrationManager::set_board(const BoardSpec& spec) {
    if (spec == d->board) {
        return 0;
    }
    d->board = spec;
#if defined(MOSAIC_HAVE_OPENCV)
    d->charucoBoard.release(); // rebuilt lazily on next feed_shot()
#endif
    const int discarded = shot_count();
    if (discarded > 0) {
        clear_shots();
    }
    return discarded;
}

void RoomCalibrationManager::set_camera_intrinsics(int cameraIndex, const CalibrationData& data) {
    d->intrinsics[cameraIndex] = data;
}

QVector<RoomCalibrationManager::CameraShotResult> RoomCalibrationManager::feed_shot(
    const QVector<VideoFrame>& frames) {
    QVector<CameraShotResult> results;

#if defined(MOSAIC_HAVE_OPENCV)
    const bool chessboard = d->board.type == BoardType::MarkedChessboard;
    // Only a ChArUco board needs its markers (and a ChArUco board object
    // refuses markers as big as the squares, a size a chessboard never uses).
    std::optional<cv::aruco::CharucoDetector> detector;
    if (!chessboard) {
        d->ensure_board();
        detector.emplace(*d->charucoBoard, cv::aruco::CharucoParameters(),
                         cv::aruco::DetectorParameters());
    }

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
            r.problem = "no lens calibration yet (Intrinsics tab)";
            results.push_back(r);
            continue;
        }

        const cv::Mat bgr(frame.height, frame.width, CV_8UC3,
                          const_cast<uint8_t*>(frame.data.data()),
                          static_cast<size_t>(frame.stride));
        cv::Mat grey;
        cv::cvtColor(bgr, grey, cv::COLOR_BGR2GRAY);

        std::vector<cv::Point3f> objPoints;
        std::vector<cv::Point2f> imgPoints;
        if (chessboard) {
            MarkedBoard found;
            const MarkedBoardStatus status = detect_marked_chessboard(
                grey, d->board.cols, d->board.rows, d->board.squareLengthMm, found);
            if (status != MarkedBoardStatus::Found) {
                r.problem = status == MarkedBoardStatus::NoBoard
                                ? "board not found (the whole board must be in view)"
                            : status == MarkedBoardStatus::OffImage
                                ? "a corner square is cut off at the image edge"
                            : status == MarkedBoardStatus::ColoursUnclear
                                ? QString(
                                      "cannot tell the black corner squares from the white "
                                      "(difference %1; more light, less glare?)")
                                      .arg(found.colourDifference, 0, 'f', 0)
                                : QString(
                                      "cannot tell which corner square is marked "
                                      "(contrast %1, next %2)")
                                      .arg(found.markContrast, 0, 'f', 0)
                                      .arg(found.runnerUpContrast, 0, 'f', 0);
                results.push_back(r);
                continue;
            }
            objPoints = std::move(found.object);
            imgPoints = std::move(found.corners);
        } else {
            cv::Mat charucoCorners, charucoIds;
            std::vector<std::vector<cv::Point2f>> markerCorners;
            std::vector<int> markerIds;
            detector->detectBoard(grey, charucoCorners, charucoIds, markerCorners, markerIds);
            if (charucoCorners.rows >= 4) {
                d->charucoBoard->matchImagePoints(charucoCorners, charucoIds, objPoints, imgPoints);
            }
            if (objPoints.size() < 4) {
                r.cornerCount = static_cast<int>(objPoints.size());
                r.problem     = "board not found (fewer than 4 ChArUco corners)";
                results.push_back(r);
                continue;
            }
        }
        r.cornerCount = static_cast<int>(objPoints.size());
        // Corners nearly in one line (a ChArUco board mostly out of view)
        // leave the board's tilt about that line undetermined.
        {
            cv::Mat pts(static_cast<int>(objPoints.size()), 2, CV_64F);
            for (int k = 0; k < pts.rows; ++k) {
                pts.at<double>(k, 0) = objPoints[static_cast<size_t>(k)].x;
                pts.at<double>(k, 1) = objPoints[static_cast<size_t>(k)].y;
            }
            cv::Mat cov, mean;
            cv::calcCovarMatrix(pts, cov, mean,
                                cv::COVAR_NORMAL | cv::COVAR_ROWS | cv::COVAR_SCALE);
            cv::Mat eig;
            cv::eigen(cov, eig);
            const double sq = d->board.squareLengthMm;
            if (eig.at<double>(1) < 0.25 * sq * sq) {
                r.problem = "the corners seen lie nearly in one line: show more of the board";
                results.push_back(r);
                continue;
            }
        }

        const CalibrationData& intr = intrinsicsIt->second;
        const cv::Mat K             = make_camera_matrix(intr.cameraMatrix);
        const cv::Mat dist          = make_dist_coeffs(intr.distCoeffs);

        // The board is flat: IPPE gives both poses a flat target can have
        // (seen from a distance, a board and its mirror tilt look alike) and
        // ranks them by fit; the better one is then refined.
        std::vector<cv::Mat> rvecs, tvecs;
        std::vector<double> errs;
        cv::Mat rvec, tvec;
        bool solved = false;
        try {
            solved = cv::solvePnPGeneric(objPoints, imgPoints, K, dist, rvecs, tvecs, false,
                                         cv::SOLVEPNP_IPPE, cv::noArray(), cv::noArray(), errs) > 0;
        } catch (const cv::Exception&) {
            solved = false;
        }
        try {
            if (solved) {
                rvec = rvecs[0];
                tvec = tvecs[0];
                cv::solvePnPRefineLM(objPoints, imgPoints, K, dist, rvec, tvec);
            } else {
                solved = cv::solvePnP(objPoints, imgPoints, K, dist, rvec, tvec, false,
                                      cv::SOLVEPNP_ITERATIVE);
            }
        } catch (const cv::Exception&) {
            solved = false;
        }
        if (!solved) {
            r.problem = "the board's position could not be solved";
            results.push_back(r);
            continue;
        }
        std::vector<cv::Point2f> projected;
        cv::projectPoints(objPoints, rvec, tvec, K, dist, projected);
        double sq = 0.0;
        for (size_t k = 0; k < projected.size(); ++k) {
            const cv::Point2f e = projected[k] - imgPoints[k];
            sq += e.x * e.x + e.y * e.y;
        }
        r.rmsPx = std::sqrt(sq / static_cast<double>(projected.size()));
        if (r.rmsPx > kMaxShotRmsPx) {
            r.problem = QString("the corners fit poorly (%1 px): blur, or a wrong board size")
                            .arg(r.rmsPx, 0, 'f', 1);
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

int RoomCalibrationManager::shot_count() const { return static_cast<int>(d->shots.size()); }

void RoomCalibrationManager::clear_shots() {
    d->shots.clear();
    d->resolved.clear();
    d->extrinsicRt.clear();
    d->reprojectionRms.clear();
    d->agreementMm.clear();
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
                int a      = shot.cameras[i].cameraIndex;
                int b      = shot.cameras[j].cameraIndex;
                Mat4 poseA = shot.cameras[i].boardToCam;
                Mat4 poseB = shot.cameras[j].boardToCam;
                if (a > b) {
                    std::swap(a, b);
                    std::swap(poseA, poseB);
                }
                auto& edge = edgeMap[{a, b}];
                edge.camA  = a;
                edge.camB  = b;
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
    d->resolved       = result.resolved;
    d->extrinsicRt    = result.extrinsicRt;

    // Per-camera reprojection RMS: how well each camera's OWN solvePnP
    // solution (from every shot where it directly saw the board) fits its
    // own detected corners — independent of BFS chain length, so this
    // reflects detection/solve quality, not accumulated chain error.
    d->reprojectionRms.assign(static_cast<size_t>(cameraCount), -1.0);
    std::vector<double> sumSqErr(static_cast<size_t>(cameraCount), 0.0);
    std::vector<int> countPts(static_cast<size_t>(cameraCount), 0);

    for (const auto& shot : d->shots) {
        for (const auto& cam : shot.cameras) {
            if (cam.cameraIndex < 0 || cam.cameraIndex >= cameraCount) {
                continue;
            }
            const auto intrIt = d->intrinsics.find(cam.cameraIndex);
            if (intrIt == d->intrinsics.end()) {
                continue;
            }

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

    // Agreement: in every shot two or more resolved cameras saw, where each
    // camera puts the board in the room, against where the others put it.
    // The board's four outer corners and centre stand for it (ChArUco views
    // may show different corners in each camera).
    const double W                                    = d->board.cols * d->board.squareLengthMm;
    const double H                                    = d->board.rows * d->board.squareLengthMm;
    const std::array<std::array<double, 3>, 5> probes = {
        {{0, 0, 0}, {W, 0, 0}, {0, H, 0}, {W, H, 0}, {W / 2, H / 2, 0}}};
    std::vector<double> agreeSq(static_cast<size_t>(cameraCount), 0.0);
    std::vector<int> agreeN(static_cast<size_t>(cameraCount), 0);
    for (const auto& shot : d->shots) {
        std::vector<std::pair<int, std::array<std::array<double, 3>, 5>>> where;
        for (const auto& cam : shot.cameras) {
            if (cam.cameraIndex < 0 || cam.cameraIndex >= cameraCount ||
                !d->resolved[static_cast<size_t>(cam.cameraIndex)]) {
                continue;
            }
            const Mat4 m = room_frame::compose(d->extrinsicRt[static_cast<size_t>(cam.cameraIndex)],
                                               cam.boardToCam);
            std::array<std::array<double, 3>, 5> pts{};
            for (size_t p = 0; p < probes.size(); ++p) {
                for (int row = 0; row < 3; ++row) {
                    pts[p][static_cast<size_t>(row)] =
                        m[static_cast<size_t>(row * 4)] * probes[p][0] +
                        m[static_cast<size_t>(row * 4 + 1)] * probes[p][1] +
                        m[static_cast<size_t>(row * 4 + 2)] * probes[p][2] +
                        m[static_cast<size_t>(row * 4 + 3)];
                }
            }
            where.emplace_back(cam.cameraIndex, pts);
        }
        if (where.size() < 2) {
            continue;
        }
        for (size_t i = 0; i < where.size(); ++i) {
            for (size_t p = 0; p < probes.size(); ++p) {
                // Leave-one-out: against the mean of the other cameras.
                std::array<double, 3> others{};
                for (size_t j = 0; j < where.size(); ++j) {
                    if (j == i) {
                        continue;
                    }
                    for (size_t k = 0; k < 3; ++k) {
                        others[k] += where[j].second[p][k] / static_cast<double>(where.size() - 1);
                    }
                }
                double sq = 0.0;
                for (size_t k = 0; k < 3; ++k) {
                    const double e = where[i].second[p][k] - others[k];
                    sq += e * e;
                }
                agreeSq[static_cast<size_t>(where[i].first)] += sq;
                ++agreeN[static_cast<size_t>(where[i].first)];
            }
        }
    }
    d->agreementMm.assign(static_cast<size_t>(cameraCount), -1.0);
    for (int i = 0; i < cameraCount; ++i) {
        if (agreeN[static_cast<size_t>(i)] > 0) {
            d->agreementMm[static_cast<size_t>(i)] =
                std::sqrt(agreeSq[static_cast<size_t>(i)] / agreeN[static_cast<size_t>(i)]);
        }
    }

    out.resolved          = d->resolved;
    out.reprojectionRmsPx = d->reprojectionRms;
    out.agreementMm       = d->agreementMm;
    out.droppedShots      = result.droppedShots;
#else
    (void)cameraCount;
    (void)referenceCameraIndex;
#endif

    return out;
}

bool RoomCalibrationManager::is_resolved(int cameraIndex) const {
    if (cameraIndex < 0 || cameraIndex >= static_cast<int>(d->resolved.size())) {
        return false;
    }
    return d->resolved[static_cast<size_t>(cameraIndex)];
}

std::array<double, 16> RoomCalibrationManager::extrinsic_for(int cameraIndex) const {
    if (cameraIndex < 0 || cameraIndex >= static_cast<int>(d->extrinsicRt.size())) {
        return room_frame::identity();
    }
    return d->extrinsicRt[static_cast<size_t>(cameraIndex)];
}

double RoomCalibrationManager::reprojection_rms_for(int cameraIndex) const {
    if (cameraIndex < 0 || cameraIndex >= static_cast<int>(d->reprojectionRms.size())) {
        return -1.0;
    }
    return d->reprojectionRms[static_cast<size_t>(cameraIndex)];
}

double RoomCalibrationManager::agreement_mm_for(int cameraIndex) const {
    if (cameraIndex < 0 || cameraIndex >= static_cast<int>(d->agreementMm.size())) {
        return -1.0;
    }
    return d->agreementMm[static_cast<size_t>(cameraIndex)];
}

bool RoomCalibrationManager::use_shot_as_plane(int shotIndex, int cameraIndex,
                                               std::array<double, 3>& outPoint,
                                               std::array<double, 3>& outNormal) const {
    if (shotIndex < 0 || shotIndex >= static_cast<int>(d->shots.size())) {
        return false;
    }
    if (!is_resolved(cameraIndex)) {
        return false;
    }

    const Shot& shot             = d->shots[static_cast<size_t>(shotIndex)];
    const ShotCameraEntry* entry = nullptr;
    for (const auto& cam : shot.cameras) {
        if (cam.cameraIndex == cameraIndex) {
            entry = &cam;
            break;
        }
    }
    if (!entry) {
        return false;
    }

    const Mat4 boardToRoom = room_frame::compose(extrinsic_for(cameraIndex), entry->boardToCam);

    outPoint = {boardToRoom[3], boardToRoom[7], boardToRoom[11]};
    // The board's printed-face normal is its local Z axis (both boards'
    // object points lie in the board's own Z=0 plane) — the 3rd column of the
    // rotation part.
    outNormal = {boardToRoom[2], boardToRoom[6], boardToRoom[10]};
    return true;
}

bool RoomCalibrationManager::use_shot_as_region(int shotIndex, int cameraIndex,
                                                std::array<double, 3>& outCentre,
                                                std::array<double, 3>& outNormal,
                                                std::array<double, 3>& outUAxis, double& outWidth,
                                                double& outHeight) const {
    std::array<double, 3> corner{};
    if (!use_shot_as_plane(shotIndex, cameraIndex, corner, outNormal)) {
        return false;
    }
    const Shot& shot = d->shots[static_cast<size_t>(shotIndex)];
    for (const auto& cam : shot.cameras) {
        if (cam.cameraIndex != cameraIndex) {
            continue;
        }
        const Mat4 boardToRoom = room_frame::compose(extrinsic_for(cameraIndex), cam.boardToCam);
        const std::array<double, 3> u = {boardToRoom[0], boardToRoom[4], boardToRoom[8]};
        const std::array<double, 3> v = {boardToRoom[1], boardToRoom[5], boardToRoom[9]};
        outWidth                      = d->board.cols * d->board.squareLengthMm;
        outHeight                     = d->board.rows * d->board.squareLengthMm;
        for (int k = 0; k < 3; ++k) {
            outCentre[static_cast<size_t>(k)] = corner[static_cast<size_t>(k)] +
                                                u[static_cast<size_t>(k)] * outWidth / 2.0 +
                                                v[static_cast<size_t>(k)] * outHeight / 2.0;
        }
        outUAxis = u;
        return true;
    }
    return false;
}

} // namespace mosaic
