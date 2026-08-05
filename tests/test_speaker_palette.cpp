#include "ui/audio/speaker_palette.hpp"
#include <gtest/gtest.h>

using mosaic::assign_speaker_palette_indices;

TEST(AssignSpeakerPaletteIndices, EmptyInputProducesEmptyMap) {
    EXPECT_TRUE(assign_speaker_palette_indices({}).isEmpty());
}

TEST(AssignSpeakerPaletteIndices, SingleLabelGetsIndexZero) {
    const auto result = assign_speaker_palette_indices({"SPEAKER_00"});
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.value("SPEAKER_00"), 0);
}

TEST(AssignSpeakerPaletteIndices, DistinctLabelsIndexedInFirstAppearanceOrder) {
    const auto result = assign_speaker_palette_indices({"SPEAKER_01", "SPEAKER_00", "SPEAKER_01"});
    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result.value("SPEAKER_01"), 0);   // seen first
    EXPECT_EQ(result.value("SPEAKER_00"), 1);   // seen second
}

TEST(AssignSpeakerPaletteIndices, RepeatedLabelReusesSameIndex) {
    const auto result = assign_speaker_palette_indices(
        {"A", "B", "A", "A", "B", "A"});
    EXPECT_EQ(result.value("A"), 0);
    EXPECT_EQ(result.value("B"), 1);
    EXPECT_EQ(result.size(), 2);
}

TEST(AssignSpeakerPaletteIndices, EmptyStringsNeverAssignedAndDontConsumeAnIndex) {
    const auto result = assign_speaker_palette_indices({"", "A", "", "B"});
    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result.value("A"), 0);
    EXPECT_EQ(result.value("B"), 1);
    EXPECT_FALSE(result.contains(""));
}

TEST(AssignSpeakerPaletteIndices, MoreThanEightDistinctLabelsKeepsIncrementingWithNoCap) {
    QStringList labels;
    for (int i = 0; i < 12; ++i) { labels << QString("SPEAKER_%1").arg(i); }
    const auto result = assign_speaker_palette_indices(labels);
    ASSERT_EQ(result.size(), 12);
    for (int i = 0; i < 12; ++i) {
        EXPECT_EQ(result.value(QString("SPEAKER_%1").arg(i)), i);
    }
}
