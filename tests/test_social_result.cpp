#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <cmath>

#include "analysis/social_result.hpp"

using mosaic::SocialResult;

namespace {

// The schema analysis/run_social.py writes (analyse()), cut down.
const char* kFixtureJson = R"JSON(
{
 "schema": "mosaic-social-v1",
 "cameras": {"a": 1, "b": 4},
 "video_start_ms": {"a": 1000.0, "b": 1012.5},
 "synchrony": {
  "heart_rate": {
   "t0_ms": 2000.0,
   "summary": {"mean_peak_r": 0.5, "surrogate_mean_peak_r": 0.45, "synchrony": 0.05,
               "p_value": 0.3, "windows": 3, "a_leads_pct": null},
   "series": {"t_s": [15.0], "peak_r": [0.5], "peak_lag_s": [-2.0]}
  },
  "head": {
   "t0_ms": 1500.0,
   "summary": {"mean_peak_r": 0.62, "surrogate_mean_peak_r": 0.41, "surrogate_p95": 0.7,
               "synchrony": 0.21, "p_value": 0.01, "windows": 3, "a_leads_pct": 80.0,
               "b_leads_pct": 20.0, "median_lag_s": 0.8},
   "series": {"t_s": [2.5, 3.0, 3.5], "peak_r": [0.7, null, 0.55], "peak_lag_s": [0.8, null, 0.6]}
  }
 },
 "mimicry": {
  "smile": {
   "a_to_b": {"events": 10, "followed": 6, "rate": 0.6, "chance_rate": 0.3, "p_value": 0.02},
   "b_to_a": {"events": 4, "followed": 0, "rate": null, "chance_rate": null, "p_value": null}
  }
 },
 "notes": ["Body movement: run Pose on Camera 2 and Camera 5."]
}
)JSON";

SocialResult load_fixture(QTemporaryDir& dir, const char* json) {
    const QString path = dir.filePath("social.json");
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(json);
    f.close();
    return SocialResult::load(path);
}

} // namespace

TEST(SocialResult, ReadsMeasuresMimicryAndNotes) {
    QTemporaryDir dir;
    const SocialResult r = load_fixture(dir, kFixtureJson);
    ASSERT_TRUE(r.is_valid());
    EXPECT_EQ(r.camera_a(), 1);
    EXPECT_EQ(r.camera_b(), 4);
    EXPECT_DOUBLE_EQ(r.video_start_ms(4), 1012.5);
    EXPECT_TRUE(std::isnan(r.video_start_ms(2)));

    // Head first, whatever the file's order; body absent.
    ASSERT_EQ(r.measures().size(), 2);
    EXPECT_EQ(r.measures()[0].id, "head");
    EXPECT_EQ(r.measure("body"), nullptr);
    const auto* head = r.measure("head");
    ASSERT_NE(head, nullptr);
    EXPECT_DOUBLE_EQ(head->t0Ms, 1500.0);
    ASSERT_EQ(head->peakR.size(), 3);
    EXPECT_TRUE(std::isnan(head->peakR[1])); // a window with too little data
    EXPECT_DOUBLE_EQ(*head->synchrony, 0.21);
    EXPECT_DOUBLE_EQ(*head->medianLagS, 0.8);
    EXPECT_FALSE(r.measure("heart_rate")->aLeadsPct.has_value()); // null stays unknown

    ASSERT_EQ(r.mimicry().size(), 2);
    EXPECT_TRUE(r.mimicry()[0].aToB);
    EXPECT_DOUBLE_EQ(*r.mimicry()[0].rate, 0.6);
    EXPECT_FALSE(r.mimicry()[1].rate.has_value());
    ASSERT_EQ(r.notes().size(), 1);
}

TEST(SocialResult, AnotherSchemaOrNoFileIsInvalid) {
    QTemporaryDir dir;
    EXPECT_FALSE(load_fixture(dir, R"({"schema": "mosaic-eye-contact-v1"})").is_valid());
    EXPECT_FALSE(SocialResult::load(dir.filePath("missing.json")).is_valid());
}
