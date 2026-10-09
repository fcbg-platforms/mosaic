#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <cmath>

#include "analysis/conversation_result.hpp"
#include "audio/audio_timing_log.hpp"

using mosaic::AudioTimingLog;
using mosaic::ConversationResult;

namespace {

// The schema analysis/run_conversation.py writes (write_outputs()), cut
// down. Turns and transitions are deliberately out of order.
const char* kFixtureJson = R"JSON(
{
 "schema": "mosaic-conversation-v1",
 "source_video": "video_2.mp4",
 "source_audio": "audio.wav",
 "camera_index": 2,
 "video_start_ms": 10150.0,
 "annotated_video": "conversation/video_2.conversation.mp4",
 "timing": {"method": "timing_file", "av_lag_ms": null, "audio_start_ms": 10062.4},
 "attribution": {"method": "diarization", "subject_label": "SPEAKER_01"},
 "summary": {
  "conversation_s": 22.8,
  "speakers": {
   "subject": {"speech_s": 12.4, "speech_pct": 54.6, "turns": 3,
               "turn_duration": {"n": 3, "median_s": 3.9},
               "pauses": 4, "pauses_per_min": 15.2, "backchannels": 0,
               "response_offset": {"n": 3, "median_s": -0.34},
               "interruptions": 1, "words": 40, "words_per_min": 192.9},
   "other": {"speech_s": 7.4, "speech_pct": 32.4, "turns": 3,
             "turn_duration": {"n": 3, "median_s": 1.8},
             "pauses": 1, "pauses_per_min": null, "backchannels": 2,
             "response_offset": {"n": 2, "median_s": 0.34},
             "interruptions": 0, "words": null, "words_per_min": null}
  },
  "transitions": {"count": 5, "floor_transfer_offset": {"n": 5, "median_s": -0.06},
                  "gaps": 2, "overlapping": 3, "interruptions": 1},
  "overlap": {"count": 2, "total_s": 1.1, "pct_of_speech": 5.0},
  "silence_pct": null,
  "coverage": {"analysed_s": 25.4, "face_seen_pct": 100.0, "unattributed_speech_s": 0.0}
 },
 "turns": [
  {"speaker": "subject", "start_ms": 16050, "end_ms": 22000, "duration_ms": 5950, "pauses": 2, "text": "Sure."},
  {"speaker": "other", "start_ms": 11060, "end_ms": 15700, "duration_ms": 4640, "pauses": 1, "text": "Thank you"}
 ],
 "transitions": [
  {"from": "subject", "to": "other", "prev_end_ms": 22000, "next_start_ms": 21940, "fto_ms": -60, "interruption": false},
  {"from": "other", "to": "subject", "prev_end_ms": 15700, "next_start_ms": 16050, "fto_ms": 350, "interruption": false}
 ],
 "spurts": [
  {"speaker": "other", "start_ms": 11060, "end_ms": 12600, "kind": "turn"},
  {"speaker": "other", "start_ms": 18000, "end_ms": 18400, "kind": "backchannel"}
 ],
 "overlaps": [[21940, 22000]],
 "signals": {"start_ms": 10150.0, "step_ms": 50, "audio_db": [-30.5, null, -2.0],
             "mouth_activity": [null, 0.02, 0.11]}
}
)JSON";

QString write_file(const QString& dir, const QByteArray& content) {
    const QString path = dir + "/video_2.conversation.json";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(content);
    return path;
}

} // namespace

TEST(ConversationResult, LoadsTurnsTransitionsSummaryAndSignals) {
    QTemporaryDir dir;
    const auto r = ConversationResult::load(write_file(dir.path(), kFixtureJson));
    ASSERT_TRUE(r.is_valid());
    EXPECT_EQ(r.source_audio(), "audio.wav");
    EXPECT_EQ(r.camera_index(), 2);
    EXPECT_DOUBLE_EQ(r.video_start_ms(), 10150.0);
    EXPECT_EQ(r.annotated_video(), "conversation/video_2.conversation.mp4");
    EXPECT_EQ(r.timing_method(), "timing_file");
    EXPECT_EQ(r.attribution_method(), "diarization");

    ASSERT_EQ(r.turns().size(), 2);
    EXPECT_EQ(r.turns()[0].speaker, "other"); // sorted by start
    EXPECT_EQ(r.turns()[1].pauses, 2);
    EXPECT_EQ(r.turns()[1].text, "Sure.");
    ASSERT_EQ(r.transitions().size(), 2);
    EXPECT_EQ(r.transitions()[0].ftoMs, 350);
    EXPECT_EQ(r.transitions()[1].to, "other");
    ASSERT_EQ(r.spurts().size(), 2);
    EXPECT_EQ(r.spurts()[1].kind, "backchannel");

    EXPECT_DOUBLE_EQ(r.subject().speechS, 12.4);
    ASSERT_TRUE(r.subject().medianResponseS.has_value());
    EXPECT_DOUBLE_EQ(*r.subject().medianResponseS, -0.34);
    EXPECT_EQ(r.subject().interruptions, 1);
    EXPECT_FALSE(r.other().wordsPerMin.has_value()); // null
    EXPECT_FALSE(r.other().pausesPerMin.has_value());
    EXPECT_EQ(r.other().backchannels, 2);
    EXPECT_EQ(r.transition_count(), 5);
    ASSERT_TRUE(r.median_fto_s().has_value());
    EXPECT_DOUBLE_EQ(*r.median_fto_s(), -0.06);
    EXPECT_EQ(r.gaps(), 2);
    EXPECT_EQ(r.overlapping_transitions(), 3);
    EXPECT_DOUBLE_EQ(r.overlap_total_s(), 1.1);
    EXPECT_FALSE(r.silence_pct().has_value());

    EXPECT_DOUBLE_EQ(r.signal_step_ms(), 50.0);
    ASSERT_EQ(r.audio_db().size(), 3);
    EXPECT_TRUE(std::isnan(r.audio_db()[1]));
    EXPECT_TRUE(std::isnan(r.mouth_activity()[0]));
    EXPECT_DOUBLE_EQ(r.mouth_activity()[2], 0.11);
}

TEST(ConversationResult, MissingOrForeignFileIsInvalid) {
    EXPECT_FALSE(ConversationResult::load("/no/such/file.json").is_valid());
    QTemporaryDir dir;
    const QString other =
        write_file(dir.path(), R"({"schema": "mosaic-face-dynamics-v1", "frames": []})");
    EXPECT_FALSE(ConversationResult::load(other).is_valid());
}

TEST(AudioTimingLog, WritesHeaderAndRowsBesideTheWav) {
    QTemporaryDir dir;
    const QString path = AudioTimingLog::path_for(dir.path() + "/audio.wav");
    EXPECT_EQ(path, dir.path() + "/audio.timing.csv");
    {
        AudioTimingLog log;
        ASSERT_TRUE(log.open(path));
        log.record(4410, 1'100'000'000);
        log.record(8820, 1'200'000'123);
    } // destructor closes
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    EXPECT_EQ(QString::fromUtf8(f.readAll()),
              "sample_count,elapsed_ns\n4410,1100000000\n8820,1200000123\n");
}
