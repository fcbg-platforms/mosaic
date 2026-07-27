#include "calibration/room_frame_solver.hpp"
#include <cmath>
#include <numbers>
#include <gtest/gtest.h>

using mosaic::room_frame::average;
using mosaic::room_frame::bfs_resolve;
using mosaic::room_frame::compose;
using mosaic::room_frame::Edge;
using mosaic::room_frame::identity;
using mosaic::room_frame::invert;
using mosaic::room_frame::Mat4;

namespace {

// Pure rotation about Z (degrees) plus a translation — a convenient
// synthetic rigid transform for these tests.
Mat4 rot_z_translate(double angleDeg, double tx, double ty, double tz) {
    const double a = angleDeg * std::numbers::pi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    return {c, -s, 0, tx,
            s,  c, 0, ty,
            0,  0, 1, tz,
            0,  0, 0, 1};
}

void expect_mat4_near(const Mat4& a, const Mat4& b, double tol = 1e-6) {
    for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(a[i], b[i], tol) << "index " << i;
    }
}

} // namespace

// ── compose / invert ─────────────────────────────────────────────────────

TEST(RoomFrameSolver, ComposeInvertRoundTrip) {
    const Mat4 t   = rot_z_translate(37.0, 1.5, -2.0, 0.25);
    const Mat4 inv = invert(t);
    expect_mat4_near(compose(t, inv), identity());
    expect_mat4_near(compose(inv, t), identity());
}

TEST(RoomFrameSolver, ComposeAppliesRightOperandFirst) {
    Mat4 a = identity();
    a[3]   = 1.0;   // translate x by 1
    Mat4 b = identity();
    b[7]   = 1.0;   // translate y by 1
    const Mat4 c = compose(a, b);
    EXPECT_NEAR(c[3], 1.0, 1e-9);
    EXPECT_NEAR(c[7], 1.0, 1e-9);
    EXPECT_NEAR(c[11], 0.0, 1e-9);
}

// The test above uses two pure translations, which commute — it can't
// actually distinguish compose(a,b) from a swapped compose(b,a), since both
// orders give the same result. A rotation composed with a translation does
// NOT commute, so this pins down the real "apply b first, then a" contract:
// translating by (1,0,0) then rotating 90 deg about Z must land at (0,1,0),
// not (1,0,0) (which is what a silently-swapped implementation would give).
TEST(RoomFrameSolver, ComposeArgumentOrderMattersWithRotation) {
    const Mat4 rot90 = rot_z_translate(90.0, 0, 0, 0);
    Mat4 translateX  = identity();
    translateX[3]    = 1.0;   // translate x by 1
    const Mat4 c = compose(rot90, translateX);
    EXPECT_NEAR(c[3], 0.0, 1e-9);
    EXPECT_NEAR(c[7], 1.0, 1e-9);
    EXPECT_NEAR(c[11], 0.0, 1e-9);
}

// ── average ───────────────────────────────────────────────────────────────

TEST(RoomFrameSolver, AverageReturnsRotationBisector) {
    const Mat4 a        = rot_z_translate(10.0, 0, 0, 0);
    const Mat4 b        = rot_z_translate(20.0, 0, 0, 0);
    const Mat4 avg      = average({a, b});
    const Mat4 expected = rot_z_translate(15.0, 0, 0, 0);
    expect_mat4_near(avg, expected, 1e-6);
}

TEST(RoomFrameSolver, AverageAveragesTranslationArithmetically) {
    Mat4 a = identity();
    a[3] = 2.0;
    Mat4 b = identity();
    b[3] = 4.0;
    const Mat4 avg = average({a, b});
    EXPECT_NEAR(avg[3], 3.0, 1e-9);
}

// The two tests above use rotations close to 180 deg, where the internal
// rotation-matrix-to-quaternion conversion happens to canonicalize both
// samples to the same hemisphere already (their dot product stays positive
// even without sign-alignment) — they'd still pass with the sign-alignment
// step deleted entirely. This test picks two rotations (239 deg and 300 deg)
// straddling that conversion's w-largest/z-largest branch boundary far
// enough apart that their raw quaternions land in OPPOSITE hemispheres
// (verified by hand: dot product is -0.86, clearly negative) — without
// sign-alignment, naively summing them would partially cancel instead of
// averaging, producing something unrelated to the correct bisector. Values
// confirmed with an independent numeric reimplementation of the conversion
// before writing this test.
TEST(RoomFrameSolver, AverageSignAlignsQuaternionsAcrossHemisphereBoundary) {
    const Mat4 a        = rot_z_translate(239.0, 0, 0, 0);
    const Mat4 b        = rot_z_translate(300.0, 0, 0, 0);
    const Mat4 avg      = average({a, b});
    // Correct short-path bisector between 239 deg and 300 deg is 269.5 deg.
    const Mat4 expected = rot_z_translate(269.5, 0, 0, 0);
    expect_mat4_near(avg, expected, 1e-6);
}

TEST(RoomFrameSolver, AverageOfSingleSampleIsUnchanged) {
    const Mat4 a = rot_z_translate(42.0, 1, 2, 3);
    expect_mat4_near(average({a}), a);
}

TEST(RoomFrameSolver, AverageOfEmptyIsIdentity) {
    expect_mat4_near(average({}), identity());
}

// ── bfs_resolve ───────────────────────────────────────────────────────────
//
// Synthetic 3-camera chain: camera 0 is the reference (identity). Cameras 1
// and 2 have known ground-truth room poses E1/E2. A single ChArUco shot,
// placed at the room origin for simplicity (T_board_room = identity), is
// shared between (0,1) and, independently, between (1,2) — but NOT directly
// between (0,2) — exercising the transitive BFS composition this module
// exists for. Per-camera board pose is boardToCam_i = invert(E_i) (since
// T_board_room = identity here).

TEST(RoomFrameSolver, ChainResolvesTransitivelyThroughIntermediateCamera) {
    const Mat4 e1 = rot_z_translate(30.0, 1.0, 0.5, 0.0);
    const Mat4 e2 = rot_z_translate(-20.0, -0.5, 2.0, 0.1);

    Edge edge01;
    edge01.camA = 0;
    edge01.camB = 1;
    edge01.boardToCamA = {invert(identity())};
    edge01.boardToCamB = {invert(e1)};

    Edge edge12;
    edge12.camA = 1;
    edge12.camB = 2;
    edge12.boardToCamA = {invert(e1)};
    edge12.boardToCamB = {invert(e2)};

    const auto result = bfs_resolve(3, 0, {edge01, edge12});

    ASSERT_TRUE(result.resolved[0]);
    ASSERT_TRUE(result.resolved[1]);
    ASSERT_TRUE(result.resolved[2]);
    expect_mat4_near(result.extrinsicRt[0], identity());
    expect_mat4_near(result.extrinsicRt[1], e1, 1e-6);
    expect_mat4_near(result.extrinsicRt[2], e2, 1e-6);
}

TEST(RoomFrameSolver, DisconnectedCameraIsReportedUnresolved) {
    const Mat4 e1 = rot_z_translate(15.0, 0.2, 0.0, 0.0);

    Edge edge01;
    edge01.camA = 0;
    edge01.camB = 1;
    edge01.boardToCamA = {invert(identity())};
    edge01.boardToCamB = {invert(e1)};

    // Camera 2 shares no shot with anything — no edge involving it at all.
    const auto result = bfs_resolve(3, 0, {edge01});

    EXPECT_TRUE(result.resolved[0]);
    EXPECT_TRUE(result.resolved[1]);
    EXPECT_FALSE(result.resolved[2]);
    expect_mat4_near(result.extrinsicRt[2], identity());
}

TEST(RoomFrameSolver, ReferenceCameraAlwaysResolvesToIdentity) {
    const auto result = bfs_resolve(1, 0, {});
    ASSERT_TRUE(result.resolved[0]);
    expect_mat4_near(result.extrinsicRt[0], identity());
}
