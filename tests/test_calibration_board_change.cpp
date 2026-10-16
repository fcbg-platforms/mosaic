// Changing the calibration board discards what was captured with the old
// one (CalibrationManager::set_board(), RoomCalibrationManager::set_board()).
// Uses real board images rendered by OpenCV, so it only builds with OpenCV.

#include <gtest/gtest.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>

#include "calibration/calibration_manager.hpp"
#include "calibration/room_calibration_manager.hpp"
#include "video/video_frame.hpp"

using mosaic::CalibrationManager;
using mosaic::RoomCalibrationManager;
using mosaic::VideoFrame;

namespace {

VideoFrame frame_from(const cv::Mat& grey, int cameraIndex) {
    cv::Mat bgr;
    cv::cvtColor(grey, bgr, cv::COLOR_GRAY2BGR);
    VideoFrame f;
    f.cameraIndex = cameraIndex;
    f.width       = bgr.cols;
    f.height      = bgr.rows;
    f.stride      = static_cast<int>(bgr.step);
    f.data.assign(bgr.data, bgr.data + bgr.total() * bgr.elemSize());
    return f;
}

/// A white-bordered image of a checkerboard with @p cols x @p rows inner corners.
cv::Mat checkerboard(int cols, int rows, int square = 40) {
    const int border = 2 * square;
    cv::Mat img((rows + 1) * square + 2 * border, (cols + 1) * square + 2 * border, CV_8UC1,
                cv::Scalar(255));
    for (int r = 0; r <= rows; ++r) {
        for (int c = 0; c <= cols; ++c) {
            if ((r + c) % 2 == 0) {
                cv::rectangle(img,
                              cv::Rect(border + c * square, border + r * square, square, square),
                              cv::Scalar(0), cv::FILLED);
            }
        }
    }
    return img;
}

/// @p img on a larger white canvas, its top-left corner at (@p x, @p y).
cv::Mat placed(const cv::Mat& img, int x, int y) {
    cv::Mat canvas(900, 1200, CV_8UC1, cv::Scalar(255));
    img.copyTo(canvas(cv::Rect(x, y, img.cols, img.rows)));
    return canvas;
}

/// The room calibration's ChArUco board (same dictionary as the manager).
cv::Mat charuco(const RoomCalibrationManager::BoardSpec& spec) {
    const cv::aruco::CharucoBoard board(
        cv::Size(spec.cols, spec.rows), static_cast<float>(spec.squareLengthMm),
        static_cast<float>(spec.markerLengthMm),
        cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_100));
    cv::Mat img;
    board.generateImage(cv::Size(spec.cols * 80, spec.rows * 80), img, 60);
    return img;
}

mosaic::CalibrationData pinhole(const cv::Mat& img) {
    mosaic::CalibrationData c;
    c.calibrated   = true;
    c.cameraMatrix = {800.0, 0.0, img.cols / 2.0, 0.0, 800.0, img.rows / 2.0, 0.0, 0.0, 1.0};
    return c;
}

} // namespace

TEST(CalibrationBoardChange, IntrinsicViewsAreDiscardedWhenTheBoardChanges) {
    CalibrationManager mgr;
    const CalibrationManager::BoardSpec spec{9, 6, 25.0};
    EXPECT_EQ(mgr.set_board(spec), 0); // the default board differs, but nothing was captured

    // Two views of the board in different places.
    ASSERT_TRUE(mgr.feed_frame(frame_from(placed(checkerboard(9, 6), 0, 0), 0)));
    ASSERT_TRUE(mgr.feed_frame(frame_from(placed(checkerboard(9, 6), 500, 300), 0)));
    ASSERT_EQ(mgr.view_count(), 2);

    EXPECT_EQ(mgr.set_board(spec), 0); // same board: views kept
    EXPECT_EQ(mgr.view_count(), 2);

    EXPECT_EQ(mgr.set_board({9, 6, 30.0}), 2); // other square size: discarded
    EXPECT_EQ(mgr.view_count(), 0);
    EXPECT_EQ(mgr.set_board({9, 6, 30.0}), 0);
}

TEST(CalibrationBoardChange, AViewOfABoardThatHasNotMovedIsNotKept) {
    CalibrationManager mgr;
    mgr.set_board({9, 6, 25.0});
    const VideoFrame f = frame_from(placed(checkerboard(9, 6), 100, 100), 0);
    ASSERT_TRUE(mgr.feed_frame(f));
    EXPECT_FALSE(mgr.feed_frame(f)); // the same pose again adds nothing
    EXPECT_EQ(mgr.view_count(), 1);
    // A frame of another size cannot join these views.
    EXPECT_FALSE(mgr.feed_frame(frame_from(checkerboard(9, 6), 0)));
    EXPECT_EQ(mgr.view_count(), 1);
    EXPECT_GT(mgr.coverage(), 0.0);
}

TEST(CalibrationBoardChange, TiltingTheBoardInPlaceGivesANewView) {
    CalibrationManager mgr;
    mgr.set_board({9, 6, 25.0});
    const cv::Mat flat = placed(checkerboard(9, 6), 300, 150);
    ASSERT_TRUE(mgr.feed_frame(frame_from(flat, 0)));
    // The same board turned about its vertical middle: the left side comes
    // nearer (taller), the right goes away (shorter); the corners barely move.
    const float top = 150, bottom = 150 + 380, l = 300, r = 300 + 560;
    const std::vector<cv::Point2f> from = {{l, top}, {r, top}, {r, bottom}, {l, bottom}};
    const std::vector<cv::Point2f> to   = {
        {l + 20, top - 35}, {r - 20, top + 35}, {r - 20, bottom - 35}, {l + 20, bottom + 35}};
    cv::Mat tilted;
    cv::warpPerspective(flat, tilted, cv::getPerspectiveTransform(from, to), flat.size(),
                        cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(255));
    EXPECT_TRUE(mgr.feed_frame(frame_from(tilted, 0)));
    EXPECT_EQ(mgr.view_count(), 2);
}

TEST(CalibrationBoardChange, RoomShotsAndTheirSolveAreDiscardedWhenTheBoardChanges) {
    RoomCalibrationManager mgr;
    const RoomCalibrationManager::BoardSpec spec{7, 5, 40.0, 30.0};
    mgr.set_board(spec);
    const cv::Mat img = charuco(spec);
    mgr.set_camera_intrinsics(0, pinhole(img));
    mgr.set_camera_intrinsics(1, pinhole(img));

    // Both cameras see the board in each shot.
    for (int k = 0; k < 2; ++k) {
        const auto results = mgr.feed_shot({frame_from(img, 0), frame_from(img, 1)});
        ASSERT_EQ(results.size(), 2);
        ASSERT_TRUE(results[0].found && results[1].found);
    }
    ASSERT_EQ(mgr.shot_count(), 2);
    mgr.solve(2, 0);
    ASSERT_TRUE(mgr.is_resolved(1));

    EXPECT_EQ(mgr.set_board(spec), 0); // unchanged: shots and solve kept
    EXPECT_EQ(mgr.shot_count(), 2);
    EXPECT_TRUE(mgr.is_resolved(1));

    RoomCalibrationManager::BoardSpec bigger = spec;
    bigger.squareLengthMm                    = 60.0;
    bigger.markerLengthMm                    = 45.0;
    EXPECT_EQ(mgr.set_board(bigger), 2);
    EXPECT_EQ(mgr.shot_count(), 0);
    EXPECT_FALSE(mgr.is_resolved(1)); // the solve came from the discarded shots
}
