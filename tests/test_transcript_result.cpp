#include "analysis/transcript_result.hpp"
#include <gtest/gtest.h>
#include <QFile>
#include <QTemporaryDir>

using mosaic::TranscriptResult;

namespace {

// Matches the exact schema written by analysis/run_diarize.py's
// process_audio(). Segments are chronological with a gap between the
// second (ends 3150ms) and third (starts 4000ms) segment.
const char* kFixtureJsonDiarized = R"JSON(
{
  "source_audio": "audio_0.wav",
  "model": "small",
  "language": "en",
  "diarization": true,
  "segments": [
    {"start_ms": 0,    "end_ms": 1500, "speaker": "SPEAKER_00", "text": "Hello there."},
    {"start_ms": 1500, "end_ms": 3150, "speaker": "SPEAKER_01", "text": "Hi, how are you?"},
    {"start_ms": 4000, "end_ms": 5200, "speaker": "SPEAKER_00", "text": "Good, thanks."}
  ]
}
)JSON";

const char* kFixtureJsonTranscriptOnly = R"JSON(
{
  "source_audio": "audio_1.wav",
  "model": "small",
  "language": "en",
  "diarization": false,
  "segments": [
    {"start_ms": 0, "end_ms": 1000, "speaker": null, "text": "No speaker labels here."}
  ]
}
)JSON";

QString write_fixture(const QString& dirPath, const QString& name, const char* json) {
    const QString path = dirPath + "/" + name;
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(json);
    return path;
}

} // namespace

TEST(TranscriptResult, LoadsValidDiarizedFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = write_fixture(dir.path(), "audio_0.transcript.json", kFixtureJsonDiarized);

    const auto result = TranscriptResult::load(path);

    ASSERT_TRUE(result.is_valid());
    EXPECT_TRUE(result.has_diarization());
    EXPECT_EQ(result.source_audio(), "audio_0.wav");
    EXPECT_EQ(result.language(), "en");

    ASSERT_EQ(result.segments().size(), 3);
    EXPECT_EQ(result.segments()[0].startMs, 0);
    EXPECT_EQ(result.segments()[0].endMs, 1500);
    EXPECT_EQ(result.segments()[0].speaker, "SPEAKER_00");
    EXPECT_EQ(result.segments()[0].text, "Hello there.");
}

TEST(TranscriptResult, TranscriptOnlyFileHasEmptySpeakerAndFalseDiarization) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
        write_fixture(dir.path(), "audio_1.transcript.json", kFixtureJsonTranscriptOnly);

    const auto result = TranscriptResult::load(path);

    ASSERT_TRUE(result.is_valid());
    EXPECT_FALSE(result.has_diarization());
    ASSERT_EQ(result.segments().size(), 1);
    EXPECT_TRUE(result.segments()[0].speaker.isEmpty());
}

TEST(TranscriptResult, MissingFileIsInvalidNotCrashing) {
    const auto result = TranscriptResult::load("Z:/does/not/exist.transcript.json");
    EXPECT_FALSE(result.is_valid());
    EXPECT_TRUE(result.segments().isEmpty());
}

TEST(TranscriptResult, SegmentAtFindsCorrectSegmentInsideRange) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = TranscriptResult::load(
        write_fixture(dir.path(), "audio_0.transcript.json", kFixtureJsonDiarized));
    ASSERT_TRUE(result.is_valid());

    const auto* seg = result.segment_at(2000);
    ASSERT_NE(seg, nullptr);
    EXPECT_EQ(seg->speaker, "SPEAKER_01");
}

TEST(TranscriptResult, SegmentAtExactStartBoundaryIsInclusive) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = TranscriptResult::load(
        write_fixture(dir.path(), "audio_0.transcript.json", kFixtureJsonDiarized));
    ASSERT_TRUE(result.is_valid());

    const auto* seg = result.segment_at(1500);
    ASSERT_NE(seg, nullptr);
    EXPECT_EQ(seg->speaker, "SPEAKER_01");
}

TEST(TranscriptResult, SegmentAtInGapBetweenSegmentsReturnsNull) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = TranscriptResult::load(
        write_fixture(dir.path(), "audio_0.transcript.json", kFixtureJsonDiarized));
    ASSERT_TRUE(result.is_valid());

    // Gap between segment 1 (ends 3150) and segment 2 (starts 4000).
    EXPECT_EQ(result.segment_at(3600), nullptr);
}

TEST(TranscriptResult, SegmentAtBeforeFirstOrAfterLastReturnsNull) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto result = TranscriptResult::load(
        write_fixture(dir.path(), "audio_0.transcript.json", kFixtureJsonDiarized));
    ASSERT_TRUE(result.is_valid());

    EXPECT_EQ(result.segment_at(-100), nullptr);
    EXPECT_EQ(result.segment_at(100000), nullptr);
}

TEST(TranscriptResult, SegmentAtOnEmptyResultReturnsNull) {
    const TranscriptResult result;
    EXPECT_EQ(result.segment_at(0), nullptr);
}
