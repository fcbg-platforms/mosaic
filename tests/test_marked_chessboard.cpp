// A plain chessboard with one marked corner square (calibration/
// marked_chessboard.hpp), and room calibration with it: board images
// rendered by OpenCV from known camera poses, so it only builds with OpenCV.

#include <gtest/gtest.h>

#include <cmath>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include "calibration/marked_chessboard.hpp"
#include "calibration/room_calibration_manager.hpp"
#include "video/video_frame.hpp"

using mosaic::detect_marked_chessboard;
using mosaic::MarkedBoard;
using mosaic::MarkedBoardStatus;
using mosaic::RoomCalibrationManager;

namespace {

constexpr int kSquares = 9;    // a 9 x 9 board, as in the room
constexpr int kPx      = 60;   // texture pixels per square
constexpr int kBorder  = 60;   // white margin around the board, texture px
constexpr double kMm   = 50.0; // one square, mm

/// The board texture: 9 x 9 squares, black in the corners, a white disc on
/// the first (top-left) corner square; or none.
cv::Mat board_texture(bool marked = true) {
    const int size = kSquares * kPx + 2 * kBorder;
    cv::Mat img(size, size, CV_8UC1, cv::Scalar(255));
    for (int r = 0; r < kSquares; ++r) {
        for (int c = 0; c < kSquares; ++c) {
            if ((r + c) % 2 == 0) {
                cv::rectangle(img, cv::Rect(kBorder + c * kPx, kBorder + r * kPx, kPx, kPx),
                              cv::Scalar(0), cv::FILLED);
            }
        }
    }
    if (marked) {
        cv::circle(img, cv::Point(kBorder + kPx / 2, kBorder + kPx / 2), kPx / 4, cv::Scalar(255),
                   cv::FILLED);
    }
    return img;
}

const cv::Matx33d kK(1000, 0, 640, 0, 1000, 480, 0, 0, 1);
const cv::Size kImage(1280, 960);

/// Board (mm, origin at the marked square's outer corner) to camera.
struct Pose {
    cv::Matx33d R;
    cv::Vec3d t;
};

Pose pose(double rx, double ry, double rz, cv::Vec3d t) {
    cv::Mat R;
    cv::Rodrigues(cv::Vec3d(rx, ry, rz) * (CV_PI / 180.0), R);
    return {cv::Matx33d(R), t};
}

/// What a camera with intrinsics kK sees of the board at @p p.
cv::Mat render(const cv::Mat& texture, const Pose& p) {
    // texture px -> board mm -> camera -> image
    const double s = kMm / kPx;
    const cv::Matx33d A(s, 0, -kBorder * s, 0, s, -kBorder * s, 0, 0, 1);
    const cv::Matx33d M(p.R(0, 0), p.R(0, 1), p.t[0], p.R(1, 0), p.R(1, 1), p.t[1], p.R(2, 0),
                        p.R(2, 1), p.t[2]);
    const cv::Matx33d H = kK * M * A;
    cv::Mat out;
    cv::warpPerspective(texture, out, cv::Mat(H), kImage, cv::INTER_AREA, cv::BORDER_CONSTANT,
                        cv::Scalar(160));
    return out;
}

/// Where the camera sees board point (x, y) mm.
cv::Point2d project(const Pose& p, double x, double y) {
    const cv::Vec3d c = p.R * cv::Vec3d(x, y, 0) + p.t;
    const cv::Vec3d h = kK * c;
    return {h[0] / h[2], h[1] / h[2]};
}

mosaic::VideoFrame frame_from(const cv::Mat& grey, int cameraIndex) {
    cv::Mat bgr;
    cv::cvtColor(grey, bgr, cv::COLOR_GRAY2BGR);
    mosaic::VideoFrame f;
    f.cameraIndex = cameraIndex;
    f.width       = bgr.cols;
    f.height      = bgr.rows;
    f.stride      = static_cast<int>(bgr.step);
    f.data.assign(bgr.data, bgr.data + bgr.total() * bgr.elemSize());
    return f;
}

} // namespace

TEST(MarkedChessboard, CornersAreNumberedFromTheMarkWhicheverWayTheBoardIsTurned) {
    const cv::Mat texture = board_texture();
    // The board turned in its own plane by quarter turns, and tilted.
    for (int turn = 0; turn < 4; ++turn) {
        const Pose p     = pose(20, -15, 90.0 * turn + 7, cv::Vec3d(-150, -120, 1400));
        const cv::Mat im = render(texture, p);
        MarkedBoard b;
        ASSERT_EQ(detect_marked_chessboard(im, kSquares, kSquares, kMm, b),
                  MarkedBoardStatus::Found)
            << "turn " << turn;
        ASSERT_EQ(b.corners.size(), 64u);
        // Every corner where the true pose puts its board point: the
        // numbering and the axes match the board's, not the image's.
        double worst = 0.0;
        for (size_t i = 0; i < b.corners.size(); ++i) {
            const cv::Point2d want = project(p, b.object[i].x, b.object[i].y);
            worst                  = std::max(worst, cv::norm(cv::Point2d(b.corners[i]) - want));
        }
        EXPECT_LT(worst, 1.0) << "turn " << turn;
        EXPECT_GT(b.markContrast, 2.0 * b.runnerUpContrast);
    }
}

TEST(MarkedChessboard, AnUnmarkedBoardIsRefused) {
    const cv::Mat im = render(board_texture(false), pose(10, 10, 5, cv::Vec3d(-200, -200, 1400)));
    MarkedBoard b;
    EXPECT_EQ(detect_marked_chessboard(im, kSquares, kSquares, kMm, b),
              MarkedBoardStatus::MarkUnclear);
}

TEST(MarkedChessboard, AWrongBoardSizeIsNotFound) {
    const cv::Mat im = render(board_texture(), pose(10, 10, 5, cv::Vec3d(-200, -200, 1400)));
    MarkedBoard b;
    EXPECT_EQ(detect_marked_chessboard(im, 8, 8, kMm, b), MarkedBoardStatus::NoBoard);
}

TEST(MarkedChessboard, RoomCalibrationRecoversTheCamerasPositions) {
    // Two cameras: camera 1 is 600 mm to the right of camera 0, turned 25
    // degrees towards it. The board is shown in four places.
    const cv::Matx33d R1w = pose(0, -25, 0, {}).R; // camera 1 axes in camera 0's frame
    const cv::Vec3d c1w(600, 0, 0);                // camera 1 centre in camera 0's frame
    const cv::Mat texture = board_texture();

    RoomCalibrationManager mgr;
    RoomCalibrationManager::BoardSpec spec;
    spec.type           = RoomCalibrationManager::BoardType::MarkedChessboard;
    spec.cols           = kSquares;
    spec.rows           = kSquares;
    spec.squareLengthMm = kMm;
    mgr.set_board(spec);
    mosaic::CalibrationData intr;
    intr.calibrated = true;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            intr.cameraMatrix[static_cast<size_t>(r * 3 + c)] = kK(r, c);
        }
    }
    mgr.set_camera_intrinsics(0, intr);
    mgr.set_camera_intrinsics(1, intr);

    const Pose boardPoses[] = {
        pose(15, 10, 3, cv::Vec3d(-100, -150, 1500)),
        pose(-10, 20, 95, cv::Vec3d(50, -100, 1700)),
        pose(5, 30, 180, cv::Vec3d(250, -50, 1600)),
        pose(-20, 15, -88, cv::Vec3d(100, 250, 1450)),
    };
    for (const Pose& b0 : boardPoses) {
        // The same board in camera 1: x1 = R1w^T (x0 - c1w).
        const Pose b1{R1w.t() * b0.R, R1w.t() * (b0.t - c1w)};
        const auto res =
            mgr.feed_shot({frame_from(render(texture, b0), 0), frame_from(render(texture, b1), 1)});
        ASSERT_EQ(res.size(), 2);
        ASSERT_TRUE(res[0].found) << res[0].problem.toStdString();
        ASSERT_TRUE(res[1].found) << res[1].problem.toStdString();
    }

    const auto solved = mgr.solve(2, 0);
    ASSERT_TRUE(mgr.is_resolved(1));
    // Camera 1 to camera 0 (the room): rotation R1w, translation c1w.
    const auto m = mgr.extrinsic_for(1);
    EXPECT_NEAR(m[3], c1w[0], 5.0);
    EXPECT_NEAR(m[7], c1w[1], 5.0);
    EXPECT_NEAR(m[11], c1w[2], 5.0);
    double maxRotErr = 0.0;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            maxRotErr =
                std::max(maxRotErr, std::abs(m[static_cast<size_t>(r * 4 + c)] - R1w(r, c)));
        }
    }
    EXPECT_LT(maxRotErr, 0.01);
    EXPECT_LT(solved.agreementMm[1], 3.0);
    EXPECT_LT(solved.agreementMm[0], 3.0);
}

TEST(MarkedChessboard, AShotWithTheBoardCutOffSaysWhy) {
    RoomCalibrationManager mgr;
    RoomCalibrationManager::BoardSpec spec;
    spec.type           = RoomCalibrationManager::BoardType::MarkedChessboard;
    spec.cols           = kSquares;
    spec.rows           = kSquares;
    spec.squareLengthMm = kMm;
    mgr.set_board(spec);
    mosaic::CalibrationData intr;
    intr.calibrated   = true;
    intr.cameraMatrix = {1000, 0, 640, 0, 1000, 480, 0, 0, 1};
    mgr.set_camera_intrinsics(0, intr);
    // Far to the left: most of the board is outside the picture.
    const cv::Mat im = render(board_texture(), pose(0, 0, 0, cv::Vec3d(-1100, -200, 1400)));
    const auto res   = mgr.feed_shot({frame_from(im, 0)});
    ASSERT_EQ(res.size(), 1);
    EXPECT_FALSE(res[0].found);
    EXPECT_FALSE(res[0].problem.isEmpty());
}
