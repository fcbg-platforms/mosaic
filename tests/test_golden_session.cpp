// The app's result readers on the golden session: the files the analysis
// plugins really write (built by analysis/tests/golden_session.py and
// committed under tests/golden/session). Every reader must accept its file,
// and what it parses must match the raw JSON. A plugin whose output changed
// in a way its reader does not follow fails here; the Python side checks that
// the output itself did not change unnoticed (analysis/tests/test_golden_session.py).

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

#include "analysis/conversation_result.hpp"
#include "analysis/eye_contact_result.hpp"
#include "analysis/face_dynamics_result.hpp"
#include "analysis/gaze2d_result.hpp"
#include "analysis/rppg_result.hpp"

using namespace mosaic;

namespace {

QString golden(const QString& rel) { return QStringLiteral(MOSAIC_GOLDEN_DIR) + "/" + rel; }

QJsonObject raw(const QString& rel) {
    QFile f(golden(rel));
    EXPECT_TRUE(f.open(QIODevice::ReadOnly)) << rel.toStdString();
    return QJsonDocument::fromJson(f.readAll()).object();
}

void expect_same(double parsed, const QJsonValue& v, const char* what) {
    if (v.isDouble()) {
        EXPECT_DOUBLE_EQ(parsed, v.toDouble()) << what;
    } else {
        EXPECT_TRUE(std::isnan(parsed)) << what << " should be NaN for null";
    }
}

} // namespace

TEST(GoldenSession, FaceDynamics) {
    const QString rel = "face_dynamics/video_2.face_dynamics.json";
    const auto r      = FaceDynamicsResult::load(golden(rel));
    ASSERT_TRUE(r.is_valid());
    const QJsonObject j = raw(rel);
    EXPECT_EQ(r.camera_index(), j["camera_index"].toInt());
    EXPECT_DOUBLE_EQ(r.fps(), j["fps"].toDouble());
    const QJsonArray frames = j["frames"].toArray();
    ASSERT_EQ(r.frames().size(), frames.size());
    for (int i = 0; i < frames.size(); i += 97) {
        const QJsonObject f = frames[i].toObject();
        EXPECT_EQ(r.frames()[i].timestampMs, f["timestamp_ms"].toInteger());
        EXPECT_EQ(r.frames()[i].faceDetected, f["face_detected"].toBool());
        if (f["face_detected"].toBool()) {
            expect_same(r.frames()[i].opennessLeft, f["openness_left"], "openness_left");
            expect_same(r.frames()[i].smile, f["smile"], "smile");
            expect_same(r.frames()[i].yaw, f["yaw"], "yaw");
            expect_same(r.frames()[i].headSpeed, f["head_speed"], "head_speed");
        }
    }
    EXPECT_EQ(r.events().size(), j["events"].toArray().size());
    const QJsonObject s = j["summary"].toObject();
    EXPECT_EQ(r.summary().blinks, s["blinks"].toObject()["count"].toInt());
    EXPECT_GT(r.summary().blinks, 0);
    EXPECT_EQ(r.summary().smiles, s["expression"].toObject()["smiles"].toInt());
    EXPECT_EQ(r.summary().duchenneSmiles, s["expression"].toObject()["duchenne_smiles"].toInt());
    EXPECT_EQ(r.summary().nods, s["head"].toObject()["nods"].toInt());
    ASSERT_TRUE(r.summary().blinksPerMinute.has_value());
    EXPECT_DOUBLE_EQ(*r.summary().blinksPerMinute, s["blinks"].toObject()["per_minute"].toDouble());
}

TEST(GoldenSession, ConversationTiming) {
    const QString rel = "conversation/video_2.conversation.json";
    const auto r      = ConversationResult::load(golden(rel));
    ASSERT_TRUE(r.is_valid());
    const QJsonObject j = raw(rel);
    EXPECT_EQ(r.attribution_method(), j["attribution"].toObject()["method"].toString());
    EXPECT_EQ(r.timing_method(), j["timing"].toObject()["method"].toString());
    EXPECT_DOUBLE_EQ(r.video_start_ms(), j["video_start_ms"].toDouble());
    ASSERT_EQ(r.turns().size(), j["turns"].toArray().size());
    ASSERT_EQ(r.transitions().size(), j["transitions"].toArray().size());
    ASSERT_EQ(r.spurts().size(), j["spurts"].toArray().size());
    EXPECT_GT(r.transitions().size(), 0);
    const QJsonObject t0 = j["transitions"].toArray()[0].toObject();
    EXPECT_EQ(r.transitions()[0].ftoMs, t0["fto_ms"].toInt());
    const QJsonObject s = j["summary"].toObject();
    EXPECT_EQ(r.transition_count(), s["transitions"].toObject()["count"].toInt());
    const QJsonObject subj = s["speakers"].toObject()["subject"].toObject();
    EXPECT_DOUBLE_EQ(r.subject().speechS, subj["speech_s"].toDouble());
    ASSERT_TRUE(r.subject().medianResponseS.has_value());
    EXPECT_DOUBLE_EQ(*r.subject().medianResponseS,
                     subj["response_offset"].toObject()["median_s"].toDouble());
    const QJsonObject sig = j["signals"].toObject();
    EXPECT_EQ(r.audio_db().size(), sig["audio_db"].toArray().size());
    EXPECT_EQ(r.mouth_activity().size(), sig["mouth_activity"].toArray().size());
}

TEST(GoldenSession, EyeContact) {
    const QString rel = "eye_contact/video_2.eye_contact.json";
    const auto r      = EyeContactResult::load(golden(rel));
    ASSERT_TRUE(r.is_valid());
    const QJsonObject j = raw(rel);
    EXPECT_EQ(r.target_method(), j["target"].toObject()["method"].toString());
    ASSERT_TRUE(r.radius_deg().has_value());
    EXPECT_DOUBLE_EQ(*r.radius_deg(), j["target"].toObject()["radius_deg"].toDouble());
    const QJsonArray frames = j["frames"].toArray();
    ASSERT_EQ(r.frames().size(), frames.size());
    int unknown = 0;
    for (int i = 0; i < frames.size(); ++i) {
        const QJsonValue c = frames[i].toObject()["contact"];
        const int expected = c.isBool() ? (c.toBool() ? 1 : 0) : -1;
        EXPECT_EQ(r.frames()[i].contact, expected) << "frame " << i;
        unknown += expected < 0;
        if (i % 101 == 0) {
            expect_same(r.frames()[i].offsetDeg, frames[i].toObject()["offset_deg"], "offset_deg");
        }
    }
    EXPECT_LT(unknown, frames.size());
    EXPECT_EQ(r.aversions().size(), j["aversions"].toArray().size());
    const QJsonObject s = j["summary"].toObject();
    EXPECT_EQ(r.summary().aversions, s["aversions"].toInt());
    ASSERT_TRUE(r.summary().listeningPct.has_value()); // the golden run has Conversation Timing
    EXPECT_DOUBLE_EQ(*r.summary().listeningPct, s["eye_contact_listening_pct"].toDouble());
    EXPECT_EQ(r.summary().subjectTurns, s["turns"].toObject()["subject_turns"].toInt());
}

TEST(GoldenSession, HeartRateAndHrv) {
    const QString rel = "rppg/video_2.pos.rppg.json";
    const auto r      = RppgResult::load(golden(rel));
    ASSERT_TRUE(r.is_valid());
    const QJsonObject j = raw(rel);
    EXPECT_EQ(r.backend(), j["backend"].toString());
    ASSERT_EQ(r.windows().size(), j["windows"].toArray().size());
    ASSERT_EQ(r.frames().size(), j["frames"].toArray().size());
    ASSERT_EQ(r.intervals().size(), j["intervals"].toArray().size());
    EXPECT_GT(r.intervals().size(), 0);
    const QJsonObject iv = j["intervals"].toArray()[3].toObject();
    EXPECT_DOUBLE_EQ(r.intervals()[3].ibiMs, iv["ibi_ms"].toDouble());
    EXPECT_EQ(r.intervals()[3].nn, iv["nn"].toBool());
    ASSERT_TRUE(r.hrv().has_value()); // 72 s at 25 fps: HRV is reported
    const QJsonObject h = j["hrv"].toObject();
    EXPECT_EQ(r.hrv()->nnCount, h["nn_count"].toInt());
    EXPECT_DOUBLE_EQ(*r.hrv()->rmssdMs, h["rmssd_ms"].toDouble());
    EXPECT_DOUBLE_EQ(*r.hrv()->rmssdCorrectedMs, h["rmssd_corrected_ms"].toDouble());
    EXPECT_DOUBLE_EQ(*r.hrv()->timingJitterMs, h["timing_jitter_ms"].toDouble());
    ASSERT_EQ(r.hrv_windows().size(), j["hrv_windows"].toArray().size());
    expect_same(r.hrv_windows()[0].rmssdMs, j["hrv_windows"].toArray()[0].toObject()["rmssd_ms"],
                "hrv window rmssd");
    const QJsonObject st = j["by_state"].toObject();
    ASSERT_TRUE(r.hr_speaking().has_value());
    EXPECT_DOUBLE_EQ(*r.hr_speaking(), st["speaking"].toObject()["mean_hr_bpm"].toDouble());
    ASSERT_TRUE(r.mean_bpm().has_value());
    EXPECT_DOUBLE_EQ(*r.mean_bpm(), j["summary"].toObject()["mean_bpm"].toDouble());
}

TEST(GoldenSession, Gaze2d) {
    const QString rel = "gaze2d/video_2.gaze2d.json";
    const auto r      = Gaze2dResult::load(golden(rel));
    ASSERT_TRUE(r.is_valid());
    const QJsonObject j = raw(rel);
    ASSERT_EQ(r.frames().size(), j["frames"].toArray().size());
    const QJsonObject f = j["frames"].toArray()[10].toObject();
    EXPECT_DOUBLE_EQ(r.frames()[10].gazeDx, f["gaze_dx"].toDouble());
    EXPECT_DOUBLE_EQ(r.pct_frames_with_face(),
                     j["summary"].toObject()["pct_frames_with_face"].toDouble());
    ASSERT_TRUE(r.pct_on_target().has_value());
    EXPECT_DOUBLE_EQ(*r.pct_on_target(), j["summary"].toObject()["pct_on_target"].toDouble());
}
