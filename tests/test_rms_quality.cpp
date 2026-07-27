#include "calibration/rms_quality.hpp"
#include <gtest/gtest.h>

using mosaic::rms_quality_for;
using mosaic::RmsQuality;

TEST(RmsQualityFor, ExcellentBelow0Point5) {
    EXPECT_EQ(rms_quality_for(0.0), RmsQuality::Excellent);
    EXPECT_EQ(rms_quality_for(0.49), RmsQuality::Excellent);
}

TEST(RmsQualityFor, GoodBetween0Point5And1) {
    EXPECT_EQ(rms_quality_for(0.5), RmsQuality::Good);
    EXPECT_EQ(rms_quality_for(0.99), RmsQuality::Good);
}

TEST(RmsQualityFor, AcceptableBetween1And2) {
    EXPECT_EQ(rms_quality_for(1.0), RmsQuality::Acceptable);
    EXPECT_EQ(rms_quality_for(1.99), RmsQuality::Acceptable);
}

TEST(RmsQualityFor, PoorAt2AndAbove) {
    EXPECT_EQ(rms_quality_for(2.0), RmsQuality::Poor);
    EXPECT_EQ(rms_quality_for(10.0), RmsQuality::Poor);
}
