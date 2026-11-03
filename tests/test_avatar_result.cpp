#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <cmath>

#include "analysis/avatar_result.hpp"

using mosaic::AvatarResult;

namespace {

// The schema analysis/run_avatar.py writes (main()), cut down.
const char* kFixtureJson = R"JSON(
{
 "schema": "mosaic-avatar-v1",
 "source": "skeleton3d.json",
 "body_model": "anny",
 "to_room": [[1000.0, 0.0, 0.0], [0.0, 0.0, -1000.0], [0.0, 1000.0, 0.0]],
 "people": [
  {"person_id": 1, "file": "avatar/person_1.bin", "vertices": 13718, "faces": 27420,
   "weights_per_vertex": 9, "ticks": 771, "bone_count": 104, "bytes": 3488832,
   "phenotype": {"age": 0.617, "height": 0.583}, "seconds": 31.2,
   "median_error_mm": 18.6, "p90_error_mm": 57.8,
   "per_joint_median_error_mm": {"nose": 19.7, "left_ankle": null},
   "per_joint_p90_error_mm": {"nose": 26.0}}
 ],
 "failed": [2]
}
)JSON";

AvatarResult load_fixture(QTemporaryDir& dir, const char* json) {
    const QString path = dir.filePath("avatar.json");
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(json);
    f.close();
    return AvatarResult::load(path);
}

} // namespace

TEST(AvatarResult, ReadsEachPersonsBodyAndFit) {
    QTemporaryDir dir;
    const AvatarResult r = load_fixture(dir, kFixtureJson);
    ASSERT_TRUE(r.is_valid());
    EXPECT_DOUBLE_EQ(r.to_room()[0], 1000.0);
    EXPECT_DOUBLE_EQ(r.to_room()[5], -1000.0); // row 1, column 2
    EXPECT_DOUBLE_EQ(r.to_room()[7], 1000.0);  // row 2, column 1
    ASSERT_EQ(r.people().size(), 1);
    const auto& p = r.people()[0];
    EXPECT_EQ(p.personId, 1);
    EXPECT_EQ(p.file, "avatar/person_1.bin");
    EXPECT_EQ(p.vertices, 13718);
    EXPECT_EQ(p.weightsPerVertex, 9);
    EXPECT_EQ(p.ticks, 771);
    EXPECT_EQ(p.boneCount, 104);
    EXPECT_EQ(p.bytes, 3488832);
    EXPECT_DOUBLE_EQ(p.seconds, 31.2);
    EXPECT_DOUBLE_EQ(p.phenotype.value("age"), 0.617);
    EXPECT_DOUBLE_EQ(p.medianErrorMm, 18.6);
    EXPECT_DOUBLE_EQ(p.p90ErrorMm, 57.8);
    EXPECT_DOUBLE_EQ(p.perJointP90ErrorMm.value("nose"), 26.0);
    EXPECT_EQ(r.failed(), QVector<int>{2});
    EXPECT_DOUBLE_EQ(p.perJointErrorMm.value("nose"), 19.7);
    EXPECT_TRUE(std::isnan(p.perJointErrorMm.value("left_ankle"))); // not measured
}

TEST(AvatarResult, AnotherSchemaOrNoFileIsInvalid) {
    QTemporaryDir dir;
    EXPECT_FALSE(load_fixture(dir, R"({"schema": "mosaic-hands-v1"})").is_valid());
    EXPECT_FALSE(load_fixture(dir, R"({"schema": "mosaic-avatar-v1"})").is_valid()); // no to_room
    EXPECT_FALSE(AvatarResult::load(dir.filePath("missing.json")).is_valid());
}
