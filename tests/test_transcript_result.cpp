#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "analysis/transcript_result.hpp"

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

// ── Diarization status ─────────────────────────────────────────────────────
//
// The reason speaker labels are missing used to exist only in the run's
// stdout, while the consequence lived on disk. These pin the persisted
// replacement, and in particular that an old sidecar cannot be mistaken for a
// successful run.

namespace {

QString write_temp(QTemporaryDir& dir, const QString& body) {
    const QString path = dir.filePath("t.transcript.json");
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(body.toUtf8());
    f.close();
    return path;
}

QString json_with_status(const QString& statusKV) {
    return QString(R"({"source_audio":"a.wav","language":"en","diarization":false,%1
                       "segments":[{"start_ms":0,"end_ms":10,"speaker":null,"text":"hi"}]})")
        .arg(statusKV);
}

} // namespace

TEST(TranscriptDiarizationStatus, ParsesEveryKnownStatus) {
    const QVector<QPair<QString, mosaic::DiarizationStatus>> cases{
        {"ok", mosaic::DiarizationStatus::Ok},
        {"skipped_by_user", mosaic::DiarizationStatus::SkippedByUser},
        {"no_token", mosaic::DiarizationStatus::NoToken},
        {"load_failed", mosaic::DiarizationStatus::LoadFailed},
        {"run_failed", mosaic::DiarizationStatus::RunFailed},
        {"no_turns", mosaic::DiarizationStatus::NoTurns},
        {"no_overlap", mosaic::DiarizationStatus::NoOverlap},
    };
    for (const auto& [text, expected] : cases) {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        const auto r = mosaic::TranscriptResult::load(
            write_temp(dir, json_with_status(QString(R"("diarization_status":"%1",)").arg(text))));
        ASSERT_TRUE(r.is_valid()) << text.toStdString();
        EXPECT_EQ(r.diarization_status(), expected) << text.toStdString();
    }
}

// THE backward-compatibility pin. A sidecar written before this field existed
// must not be read as a successful diarization — that would suppress the very
// banner explaining why the Speaker column is empty.
TEST(TranscriptDiarizationStatus, FileWithoutTheKeyIsUnknownNotOk) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto r = mosaic::TranscriptResult::load(write_temp(dir, json_with_status("")));
    ASSERT_TRUE(r.is_valid());
    EXPECT_EQ(r.diarization_status(), mosaic::DiarizationStatus::Unknown);
    EXPECT_NE(r.diarization_status(), mosaic::DiarizationStatus::Ok);
}

// A status this build doesn't know must degrade, never alias onto one it does.
TEST(TranscriptDiarizationStatus, UnrecognisedStatusIsUnknown) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto r = mosaic::TranscriptResult::load(
        write_temp(dir, json_with_status(R"("diarization_status":"from_the_future",)")));
    ASSERT_TRUE(r.is_valid());
    EXPECT_EQ(r.diarization_status(), mosaic::DiarizationStatus::Unknown);
}

// For a gated-model failure the verbatim Python message is the only thing that
// separates "bad token" from "token fine, licence not accepted".
TEST(TranscriptDiarizationStatus, DetailSurvivesVerbatim) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto r = mosaic::TranscriptResult::load(
        write_temp(dir, json_with_status(R"("diarization_status":"load_failed",
                                  "diarization_detail":"401 Client Error: Gated repo",)")));
    ASSERT_TRUE(r.is_valid());
    EXPECT_TRUE(r.diarization_detail().contains("401"));
}

TEST(TranscriptDiarizationStatus, HeadlineIsEmptyOnlyWhenThereIsNothingToReport) {
    using mosaic::diarization_status_headline;
    using mosaic::DiarizationStatus;

    EXPECT_TRUE(diarization_status_headline(DiarizationStatus::Ok, true).isEmpty());
    // A legacy file that says diarization ran is taken at its word.
    EXPECT_TRUE(diarization_status_headline(DiarizationStatus::Unknown, true).isEmpty());
    // A legacy file with no labels still warrants saying so, without a cause.
    EXPECT_FALSE(diarization_status_headline(DiarizationStatus::Unknown, false).isEmpty());

    for (const auto s : {DiarizationStatus::SkippedByUser, DiarizationStatus::NoToken,
                         DiarizationStatus::LoadFailed, DiarizationStatus::RunFailed,
                         DiarizationStatus::NoTurns, DiarizationStatus::NoOverlap}) {
        EXPECT_FALSE(diarization_status_headline(s, false).isEmpty());
    }
}

// These two are the pair most easily collapsed into one status, and doing so
// produced a banner that contradicted its own detail line: "found no speaker
// turns" printed directly above "Found 4 speaker turn(s), but none
// overlapped". They must stay distinguishable, and the advice must differ —
// telling someone to check the microphone is wrong when it plainly did record
// speech.
TEST(TranscriptDiarizationStatus, NoTurnsAndNoOverlapDoNotSayTheSameThing) {
    using mosaic::diarization_status_headline;
    using mosaic::diarization_status_remedy;
    using mosaic::DiarizationStatus;

    EXPECT_NE(diarization_status_headline(DiarizationStatus::NoTurns, false),
              diarization_status_headline(DiarizationStatus::NoOverlap, false));
    EXPECT_NE(diarization_status_remedy(DiarizationStatus::NoTurns),
              diarization_status_remedy(DiarizationStatus::NoOverlap));

    // Only the genuinely-heard-nobody case should point at the microphone.
    EXPECT_TRUE(diarization_status_remedy(DiarizationStatus::NoTurns)
                    .contains("microphone", Qt::CaseInsensitive));
    EXPECT_FALSE(diarization_status_remedy(DiarizationStatus::NoOverlap)
                     .contains("microphone", Qt::CaseInsensitive));
}

// The remedy is the half a bare error message always omits, so the two statuses
// a user can actually act on must name both required steps.
TEST(TranscriptDiarizationStatus, TokenRemediesNameTheGatedModelToo) {
    for (const auto s :
         {mosaic::DiarizationStatus::NoToken, mosaic::DiarizationStatus::LoadFailed}) {
        const QString remedy = mosaic::diarization_status_remedy(s);
        EXPECT_TRUE(remedy.contains("token", Qt::CaseInsensitive));
        EXPECT_TRUE(remedy.contains("pyannote/speaker-diarization-community-1"));
    }
    EXPECT_TRUE(mosaic::diarization_status_remedy(mosaic::DiarizationStatus::Ok).isEmpty());
}
