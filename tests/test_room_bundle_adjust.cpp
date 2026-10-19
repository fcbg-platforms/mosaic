// Joint refinement of the room calibration (calibration/room_bundle_adjust.hpp)
// on corners projected through cameras whose true poses are known.

#include <gtest/gtest.h>

#include <cmath>
#include <opencv2/calib3d.hpp>
#include <random>

#include "calibration/room_bundle_adjust.hpp"

using mosaic::room_ba::Lens;
using mosaic::room_ba::Observation;
using mosaic::room_ba::refine;
using mosaic::room_frame::Mat4;
namespace rf = mosaic::room_frame;

namespace {

const cv::Matx33d kK(1400, 0, 960, 0, 1400, 540, 0, 0, 1);

Mat4 mat4(const cv::Matx33d& R, const cv::Vec3d& t) {
    return {R(0, 0), R(0, 1), R(0, 2), t[0], R(1, 0), R(1, 1), R(1, 2), t[1],
            R(2, 0), R(2, 1), R(2, 2), t[2], 0,       0,       0,       1};
}

/// Camera to room for a camera at @p pos looking at the room origin
/// (OpenCV axes: z forward, y down).
Mat4 look_at(const cv::Vec3d& pos) {
    const cv::Vec3d z = cv::normalize(-pos);
    const cv::Vec3d x = cv::normalize(cv::Vec3d(0, 1, 0).cross(z) * -1.0);
    const cv::Vec3d y = z.cross(x);
    const cv::Matx33d R(x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2]);
    return mat4(R, pos);
}

Mat4 rotate_about(const Mat4& m, const cv::Vec3d& rvecDeg, const cv::Vec3d& shift) {
    cv::Matx33d dR;
    cv::Rodrigues(rvecDeg * (CV_PI / 180.0), dR);
    const cv::Matx33d R(m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
    return mat4(dR * R, cv::Vec3d(m[3], m[7], m[11]) + shift);
}

double centre_error_mm(const Mat4& a, const Mat4& b) {
    return std::hypot(a[3] - b[3], a[7] - b[7], a[11] - b[11]);
}

struct Scene {
    std::vector<Mat4> truth; // camera to room
    std::map<int, Lens> lenses;
    std::vector<Observation> obs;
};

/// Four cameras 2.5 m away in front of a 9 x 8 board (54 mm squares) shown
/// in 12 places and tilts; each view's own PnP pose, as the app stores it.
Scene make_scene(double noisePx) {
    Scene s;
    s.truth = {look_at({0, -300, -2500}), look_at({1400, -400, -2100}),
               look_at({-1400, -200, -2100}), look_at({600, -900, -2400})};
    for (int c = 0; c < 4; ++c) {
        s.lenses[c] = {kK, cv::Mat::zeros(5, 1, CV_64F)};
    }
    std::vector<cv::Point3f> board;
    for (int r = 0; r < 7; ++r) {
        for (int c = 0; c < 8; ++c) {
            board.emplace_back((c + 1) * 54.0f, (r + 1) * 54.0f, 0.0f);
        }
    }
    std::mt19937 rng(3);
    std::normal_distribution<double> noise(0.0, noisePx);
    for (int shot = 0; shot < 12; ++shot) {
        // Board to room: centred near the origin, tilted a different way each time.
        cv::Matx33d R;
        cv::Rodrigues(cv::Vec3d(0.3 * std::sin(shot * 1.7), 0.35 * std::cos(shot * 1.1),
                                0.5 * std::sin(shot * 0.6)),
                      R);
        const cv::Vec3d t =
            cv::Vec3d(-240.0 + 60.0 * (shot % 4), -200.0 + 50.0 * (shot % 3), 100.0 * (shot % 2)) -
            R * cv::Vec3d(243, 216, 0);
        const Mat4 boardToRoom = mat4(R, t);
        for (int c = 0; c < 4; ++c) {
            const Mat4 boardToCam =
                rf::compose(rf::invert(s.truth[static_cast<size_t>(c)]), boardToRoom);
            const cv::Matx33d Rc(boardToCam[0], boardToCam[1], boardToCam[2], boardToCam[4],
                                 boardToCam[5], boardToCam[6], boardToCam[8], boardToCam[9],
                                 boardToCam[10]);
            cv::Vec3d rvec;
            cv::Rodrigues(Rc, rvec);
            const cv::Vec3d tvec(boardToCam[3], boardToCam[7], boardToCam[11]);
            std::vector<cv::Point2f> px;
            cv::projectPoints(board, rvec, tvec, kK, cv::noArray(), px);
            for (auto& p : px) {
                p.x += static_cast<float>(noise(rng));
                p.y += static_cast<float>(noise(rng));
            }
            cv::Vec3d r2, t2;
            cv::solvePnP(board, px, kK, cv::noArray(), r2, t2);
            cv::Matx33d R2;
            cv::Rodrigues(r2, R2);
            s.obs.push_back({shot, c, board, px, mat4(R2, t2)});
        }
    }
    return s;
}

/// The true poses, each non-reference camera knocked off by a few degrees
/// and tens of mm, as a chain of averaged pairwise poses might leave them.
std::vector<Mat4> knocked(const std::vector<Mat4>& truth) {
    std::vector<Mat4> m = truth;
    m[1]                = rotate_about(m[1], {1.5, -1.0, 0.5}, {25, -10, 30});
    m[2]                = rotate_about(m[2], {-0.8, 1.2, -1.0}, {-30, 15, -20});
    m[3]                = rotate_about(m[3], {1.0, 0.7, 1.5}, {10, 35, -25});
    return m;
}

} // namespace

TEST(RoomBundleAdjust, RecoversTheCamerasFromKnockedPoses) {
    const Scene s = make_scene(0.2);
    const auto r  = refine(4, 0, {true, true, true, true}, knocked(s.truth), s.lenses, s.obs);
    ASSERT_TRUE(r.applied);
    EXPECT_EQ(r.shotsUsed, 12);
    for (size_t c = 1; c < 4; ++c) {
        EXPECT_LT(centre_error_mm(r.cameraToRoom[c], s.truth[c]), 2.0) << "camera " << c;
        EXPECT_LT(rf::rotation_angle_deg(r.cameraToRoom[c], s.truth[c]), 0.1) << "camera " << c;
        EXPECT_LT(r.rmsAfterPx[c], 0.35) << "camera " << c;
    }
    EXPECT_GT(r.totalRmsBeforePx, 2.0 * r.totalRmsAfterPx);
    // The reference camera stays where it was.
    EXPECT_EQ(r.cameraToRoom[0], s.truth[0]);
    EXPECT_EQ(r.outlierCorners, 0);
}

TEST(RoomBundleAdjust, OneBadViewDoesNotPullTheRoom) {
    Scene s = make_scene(0.2);
    // Camera 2's view of shot 5: every corner 15 px off (a misdetection).
    for (auto& o : s.obs) {
        if (o.shot == 5 && o.camera == 2) {
            for (auto& p : o.image) {
                p.x += 15.0f;
                p.y -= 8.0f;
            }
        }
    }
    const auto r = refine(4, 0, {true, true, true, true}, knocked(s.truth), s.lenses, s.obs);
    ASSERT_TRUE(r.applied);
    for (size_t c = 1; c < 4; ++c) {
        EXPECT_LT(centre_error_mm(r.cameraToRoom[c], s.truth[c]), 3.0) << "camera " << c;
    }
    EXPECT_EQ(r.droppedViews, 1); // that one view, and no other
}

TEST(RoomBundleAdjust, UnresolvedCamerasAndLoneShotsAreLeftAlone) {
    const Scene s       = make_scene(0.2);
    std::vector<Mat4> m = knocked(s.truth);
    // Camera 3 unresolved: kept exactly as given, and never reported.
    const auto r = refine(4, 0, {true, true, true, false}, m, s.lenses, s.obs);
    ASSERT_TRUE(r.applied);
    EXPECT_EQ(r.cameraToRoom[3], m[3]);
    EXPECT_LT(r.rmsAfterPx[3], 0.0);
    // Only the reference camera resolved: nothing to refine.
    const auto alone = refine(4, 0, {true, false, false, false}, m, s.lenses, s.obs);
    EXPECT_FALSE(alone.applied);
    EXPECT_EQ(alone.shotsUsed, 0);
}

TEST(RoomBundleAdjust, FewViewsAreNeverDropped) {
    // Two cameras, four shots: eight views, under the ten needed to drop any.
    Scene s = make_scene(0.2);
    std::vector<Observation> few;
    for (const auto& o : s.obs) {
        if (o.camera < 2 && o.shot < 4) {
            few.push_back(o);
        }
    }
    for (auto& p : few[0].image) {
        p.x += 15.0f;
    }
    const auto r = refine(2, 0, {true, true}, {s.truth[0], s.truth[1]}, s.lenses, few);
    EXPECT_EQ(r.droppedViews, 0);
    EXPECT_EQ(r.droppedPerCamera, std::vector<int>({0, 0}));
}
