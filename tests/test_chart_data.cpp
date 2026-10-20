// The numbers behind the Analysis tab's distribution and comparison charts
// (analysis/chart_data.hpp).

#include <gtest/gtest.h>

#include <cmath>

#include "analysis/chart_data.hpp"

using namespace mosaic;

namespace {

ConversationSpurt spurt(const char* who, int64_t a, int64_t b) {
    ConversationSpurt s;
    s.speaker = who;
    s.startMs = a;
    s.endMs   = b;
    s.kind    = "turn";
    return s;
}

RppgInterval beat(int64_t t, double ibi, bool nn = true) {
    RppgInterval i;
    i.tMs   = t;
    i.ibiMs = ibi;
    i.nn    = nn;
    return i;
}

} // namespace

TEST(ChartData, BinsAndLabels) {
    const QVector<double> edges = {0.0, 0.5, 1.0, 2.0};
    EXPECT_EQ(bin_counts({0.2, 0.5, 0.9, 1.5, 3.0, 40.0, -1.0, NAN}, edges),
              QVector<double>({2.0, 2.0, 1.0, 2.0})); // -1 in the first bin, NaN dropped
    EXPECT_EQ(bin_labels(edges), QStringList({"0-0.5", "0.5-1", "1-2", "2+"}));
}

TEST(ChartData, SpeakingSharesSplitOverlapAndSilence) {
    // On camera 0-4 s, other 3-6 s and 8-9 s, over 0-10 s.
    const auto s = speaking_shares(
        {spurt("subject", 0, 4000), spurt("other", 3000, 6000), spurt("other", 8000, 9000)}, 0,
        10000);
    EXPECT_DOUBLE_EQ(s.bothS, 1.0);
    EXPECT_DOUBLE_EQ(s.subjectOnlyS, 3.0);
    EXPECT_DOUBLE_EQ(s.otherOnlyS, 3.0);
    EXPECT_DOUBLE_EQ(s.silenceS, 3.0);
    EXPECT_DOUBLE_EQ(s.subjectOnlyS + s.otherOnlyS + s.bothS + s.silenceS, 10.0);
}

TEST(ChartData, HeartRateBySpeakingListeningSilence) {
    QVector<RppgInterval> beats;
    // 1 s beats (60 bpm) at 0-14 s, 0.75 s (80 bpm) at 20-34 s, 0.6 s (100) after.
    for (int t = 1000; t <= 14000; t += 1000) {
        beats << beat(t, 1000.0);
    }
    for (int t = 20750; t <= 34000; t += 750) {
        beats << beat(t, 750.0);
    }
    for (int t = 40600; t <= 50000; t += 600) {
        beats << beat(t, 600.0);
    }
    beats << beat(15000, 300.0, false); // an artifact: never counted
    const auto h = hr_by_state(beats, {spurt("subject", 0, 15000), spurt("other", 20000, 35000)});
    ASSERT_TRUE(h.speakingBpm && h.listeningBpm && h.silenceBpm);
    EXPECT_NEAR(*h.speakingBpm, 60.0, 1e-9);
    EXPECT_NEAR(*h.listeningBpm, 80.0, 1e-9);
    EXPECT_NEAR(*h.silenceBpm, 100.0, 1e-9);
    // Overlapping speech counts as both speaking and listening.
    const auto both = hr_by_state(beats, {spurt("subject", 0, 15000), spurt("other", 0, 15000)});
    EXPECT_EQ(both.speakingBeats, 14);
    EXPECT_EQ(both.listeningBeats, 14);
    // Too few beats in a state: unknown, not a guess.
    EXPECT_FALSE(hr_by_state(beats.mid(0, 5), {spurt("subject", 0, 15000)}).speakingBpm);
}

TEST(ChartData, PoincarePairsConsecutiveCleanBeats) {
    // Alternating 800/900 ms: SD1 large (beat to beat), SD2 small.
    QVector<RppgInterval> beats;
    int64_t t = 0;
    for (int k = 0; k < 20; ++k) {
        const double ibi = k % 2 ? 900.0 : 800.0;
        t += static_cast<int64_t>(ibi);
        beats << beat(t, ibi, k != 10); // one artifact breaks two pairs
    }
    const auto p = poincare(beats);
    EXPECT_EQ(p.points.size(), 17);
    ASSERT_TRUE(p.sd1Ms && p.sd2Ms);
    EXPECT_GT(*p.sd1Ms, 60.0);
    EXPECT_LT(*p.sd2Ms, *p.sd1Ms);
    EXPECT_FALSE(poincare(beats.mid(0, 2)).sd1Ms);
}

TEST(ChartData, BlinkRatePerMinuteOfFaceSeen) {
    // 90 s at 10 frames per second; the face is gone from 30 to 60 s, and
    // seen only half the time (every other frame) from 60 to 90 s.
    QVector<FaceDynamicsFrame> frames;
    for (int k = 0; k <= 900; ++k) {
        FaceDynamicsFrame f;
        f.timestampMs  = 100 * k;
        f.faceDetected = k < 300 || (k >= 600 && k % 2 == 0);
        frames << f;
    }
    QVector<FaceDynamicsEvent> ev;
    for (int t : {1000, 5000, 9000, 70000}) {
        FaceDynamicsEvent e;
        e.kind   = "blink";
        e.peakMs = t;
        ev << e;
    }
    FaceDynamicsEvent smile;
    smile.kind   = "smile";
    smile.peakMs = 2000;
    ev << smile;
    const auto r = blink_rate(ev, frames);
    ASSERT_EQ(r.size(), 3);
    EXPECT_DOUBLE_EQ(r[0].y(), 6.0);   // 3 blinks in 30 s of face
    EXPECT_TRUE(std::isnan(r[1].y())); // no face: a gap, not zero
    EXPECT_DOUBLE_EQ(r[2].y(), 4.0);   // 1 blink in 15 s of face
    // A 40 s recording: one window, not a 30 s and a 10 s one.
    EXPECT_EQ(blink_rate(ev, frames.mid(0, 401)).size(), 1);
}

TEST(ChartData, SpeechRatePerTurn) {
    ConversationTurn a;
    a.speaker          = "subject";
    a.startMs          = 0;
    a.endMs            = 3000;
    a.text             = "one two three four five six";
    ConversationTurn b = a;
    b.speaker          = "other";
    b.startMs          = 4000;
    b.endMs            = 4500; // under a second: no rate
    ConversationTurn c = a;
    c.text             = ""; // no transcript: no rate
    const auto r       = speech_rates({a, b, c});
    ASSERT_EQ(r.subject.size(), 1);
    EXPECT_DOUBLE_EQ(r.subject[0].x(), 1500.0);
    EXPECT_DOUBLE_EQ(r.subject[0].y(), 120.0); // 6 words in 3 s
    EXPECT_TRUE(r.other.isEmpty());
}

TEST(ChartData, ExpressionSharesAndActionUnits) {
    auto frame = [](const char* label, QVector<double> aus) {
        ExpressionFrame f;
        ExpressionSubject s;
        s.dominantExpression = label;
        s.actionUnits        = aus;
        f.subjects << s;
        return f;
    };
    QVector<ExpressionFrame> frames = {frame("happy", {0.2, 0.8}), frame("happy", {0.4, 0.6}),
                                       frame("neutral", {0.0, 0.1}), ExpressionFrame{},
                                       frame("", {})};
    const auto shares               = expression_shares(frames);
    ASSERT_EQ(shares.size(), 3); // happy, neutral and "none"; a frame without a face not at all
    EXPECT_EQ(shares[0].first, "happy"); // largest first
    EXPECT_NEAR(shares[0].second, 50.0, 1e-9);
    const auto aus = mean_action_units(frames, 2);
    ASSERT_EQ(aus.size(), 2);
    EXPECT_NEAR(aus[0], 0.2, 1e-9);
    EXPECT_NEAR(aus[1], 0.5, 1e-9);
    EXPECT_TRUE(mean_action_units(frames, 0).isEmpty());
}
