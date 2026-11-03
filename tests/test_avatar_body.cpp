#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include "analysis/avatar_body.hpp"
#include "avatar_fixture.hpp"

using mosaic::AvatarBody;

namespace {

constexpr std::array<double, 12> kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

} // namespace

TEST(AvatarBody, ReadsTheFileAndSkinsItsVertices) {
    QTemporaryDir dir;
    const auto body = AvatarBody::load(write_tiny_avatar(dir.filePath("p.bin")),
                                       tiny_avatar_person(), {"root", "lowerleg01.L"});
    ASSERT_TRUE(body.is_valid());
    EXPECT_EQ(body.vertex_count(), 3);
    EXPECT_EQ(body.tick_count(), 2);
    std::vector<float> pos, nrm;
    body.pose(0, kIdentity, pos, nrm);
    // Vertex 0 on bone 0 (at rest); vertex 1 half on bone 1 (moved +2 in x);
    // vertex 2 all on bone 1.
    EXPECT_FLOAT_EQ(pos[0], 0.0f);
    EXPECT_FLOAT_EQ(pos[3], 2.0f); // 0.5 * 1 + 0.5 * (1 + 2)
    EXPECT_FLOAT_EQ(pos[6], 2.0f);
    EXPECT_FLOAT_EQ(pos[7], 1.0f);
    // The triangle's normal, at every corner: +z, out of its front.
    for (int v = 0; v < 3; ++v) {
        EXPECT_NEAR(nrm[3 * v + 2], 1.0f, 1e-5f);
    }
    // Tick 1: bone 0 turned 90 degrees about z takes vertex 1's share
    // (1, 0, 0) to (0, 1, 0); the frame's 3 x 4 (here x 1000, then +5 in y)
    // applies to the result.
    const std::array<double, 12> toScene = {1000, 0, 0, 0, 0, 1000, 0, 5, 0, 0, 1000, 0};
    body.pose(1, toScene, pos, nrm);
    EXPECT_NEAR(pos[3], 500.0f, 1e-3f);        // 0.5 * 0 + 0.5 * 1 (bone 1 at rest)
    EXPECT_NEAR(pos[4], 500.0f + 5.0f, 1e-3f); // 0.5 * 1 + 0.5 * 0, plus 5
}

TEST(AvatarBody, FindsTheTickAndWhatWasMeasured) {
    QTemporaryDir dir;
    const auto body = AvatarBody::load(write_tiny_avatar(dir.filePath("p.bin")),
                                       tiny_avatar_person(), {"root", "lowerleg01.L"});
    ASSERT_TRUE(body.is_valid());
    EXPECT_EQ(body.nearest_tick(1015.0), 0);
    EXPECT_EQ(body.nearest_tick(1030.0), 1);
    EXPECT_EQ(body.nearest_tick(5000.0), -1); // over 100 ms from any tick
    EXPECT_EQ(body.nearest_tick(5000.0, 1e9), 1);
    // Vertices 1 and 2 move mostly with bone 1 (a left lower leg); vertex 0
    // with the root.
    using R = AvatarBody::Region;
    EXPECT_EQ(body.regions()[0], R::Body);
    EXPECT_EQ(body.regions()[2], R::LeftLowerLeg);
    EXPECT_TRUE(body.measured(0, R::LeftLowerLeg));
    EXPECT_FALSE(body.measured(1, R::LeftLowerLeg)); // its ankle not found then
    EXPECT_TRUE(body.measured(1, R::RightLowerLeg));
    EXPECT_TRUE(body.measured(1, R::Body));
    EXPECT_FALSE(body.measured(7, R::Body)); // no such tick
}

TEST(AvatarBody, AFileOfAnotherSizeIsInvalid) {
    QTemporaryDir dir;
    EXPECT_FALSE(
        AvatarBody::load(write_tiny_avatar(dir.filePath("short.bin"), 4), tiny_avatar_person(), {})
            .is_valid());
    EXPECT_FALSE(
        AvatarBody::load(dir.filePath("missing.bin"), tiny_avatar_person(), {}).is_valid());
    auto more  = tiny_avatar_person();
    more.ticks = 3;
    EXPECT_FALSE(AvatarBody::load(write_tiny_avatar(dir.filePath("p.bin")), more, {}).is_valid());
    // Counts from a damaged avatar.json: none, or too large to multiply.
    auto none             = tiny_avatar_person();
    none.weightsPerVertex = 0;
    EXPECT_FALSE(AvatarBody::load(dir.filePath("p.bin"), none, {}).is_valid());
    auto huge      = tiny_avatar_person();
    huge.ticks     = 2147483647;
    huge.boneCount = 200000000;
    EXPECT_FALSE(AvatarBody::load(dir.filePath("p.bin"), huge, {}).is_valid());
    // A bone that is not there: one bone, its transforms trimmed to fit,
    // while the skinning names bone 1.
    auto fewerBones      = tiny_avatar_person();
    fewerBones.boneCount = 1;
    EXPECT_FALSE(
        AvatarBody::load(write_tiny_avatar(dir.filePath("b.bin"), 2 * 7 * 4), fewerBones, {})
            .is_valid());
}

TEST(AvatarBody, AResultWithoutBoneNamesSaysToRunAgain) {
    QTemporaryDir dir;
    write_tiny_avatar_session(dir.path());
    QFile json(dir.filePath("avatar/avatar.json"));
    ASSERT_TRUE(json.open(QIODevice::ReadOnly));
    QByteArray text = json.readAll();
    json.close();
    text.replace(R"("bone_labels": ["root", "lowerleg01.L"],)", "");
    ASSERT_TRUE(json.open(QIODevice::WriteOnly));
    json.write(text);
    json.close();
    const auto avatars = mosaic::load_session_avatars(dir.path());
    ASSERT_EQ(avatars.bodies.size(), 1); // still drawn, all of it as measured
    EXPECT_EQ(avatars.bodies[0].regions()[2], AvatarBody::Region::Body);
    EXPECT_TRUE(avatars.note.contains("which limbs were estimated"));
}

TEST(AvatarBody, SessionAvatarsAreLeftOutWhenOlderThanThe3DPoseResult) {
    QTemporaryDir dir;
    write_tiny_avatar_session(dir.path());
    auto fine = mosaic::load_session_avatars(dir.path());
    ASSERT_EQ(fine.bodies.size(), 1);
    EXPECT_TRUE(fine.bodies[0].is_valid());
    EXPECT_TRUE(fine.note.isEmpty());
    // A 3D Pose result written after the avatars: they are not shown.
    QThread::msleep(20);
    QFile pose(dir.filePath("skeleton3d.json"));
    ASSERT_TRUE(pose.open(QIODevice::WriteOnly));
    pose.write("{}");
    pose.close();
    const auto stale = mosaic::load_session_avatars(dir.path());
    EXPECT_TRUE(stale.bodies.isEmpty());
    EXPECT_TRUE(stale.note.contains("run 3D Avatar again"));
    EXPECT_TRUE(mosaic::load_session_avatars(dir.filePath("nowhere")).bodies.isEmpty());
}
