#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "analysis/clap_check_result.hpp"

using mosaic::ClapCheckResult;

namespace {

ClapCheckResult load_fixture(QTemporaryDir& dir, const char* json) {
    const QString path = dir.filePath("clap_check.json");
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(json);
    f.close();
    return ClapCheckResult::load(path);
}

} // namespace

TEST(ClapCheckResult, ReadsEveryCameraAndMicrophone) {
    QTemporaryDir dir;
    const auto r = load_fixture(dir, R"JSON(
{"schema": "mosaic-clap-check-v1", "verdict": "check",
 "cameras": [
  {"camera": 2, "frame_ms": 40.0, "microphones": [
    {"file": "audio.wav", "matched": 3, "median_offset_ms": 31.0, "spread_ms": 2.0,
     "verdict": "check"}]},
  {"camera": 4, "frame_ms": 40.0, "microphones": [
    {"file": "audio.wav", "matched": 0, "median_offset_ms": null, "spread_ms": null,
     "verdict": "none"}]}
 ]})JSON");
    ASSERT_TRUE(r.is_valid());
    EXPECT_EQ(r.verdict(), "check");
    ASSERT_EQ(r.pairs().size(), 2);
    EXPECT_DOUBLE_EQ(*r.pairs()[0].medianOffsetMs, 31.0);
    EXPECT_FALSE(r.pairs()[1].medianOffsetMs.has_value());
    EXPECT_EQ(r.summary(),
              "Camera 3 / audio.wav: audio +31 ms from the video over 3 clap(s) "
              "(check)  ·  Camera 5 / audio.wav: no clap both heard and seen");
}

TEST(ClapCheckResult, AnotherSchemaOrNoFileIsInvalid) {
    QTemporaryDir dir;
    EXPECT_FALSE(load_fixture(dir, R"({"schema": "mosaic-social-v1"})").is_valid());
    EXPECT_FALSE(ClapCheckResult::load(dir.filePath("missing.json")).is_valid());
}
