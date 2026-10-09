#include "analysis/conversation_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <limits>

namespace mosaic {

namespace {

constexpr auto kSchema = "mosaic-conversation-v1";

std::optional<double> optional_double(const QJsonValue& v) {
    return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
}

QVector<double> number_array(const QJsonValue& v) {
    QVector<double> out;
    for (const auto& x : v.toArray()) {
        out << (x.isDouble() ? x.toDouble() : std::numeric_limits<double>::quiet_NaN());
    }
    return out;
}

ConversationSpeakerStats speaker_stats(const QJsonObject& o) {
    ConversationSpeakerStats s;
    s.speechS         = o["speech_s"].toDouble();
    s.speechPct       = optional_double(o["speech_pct"]);
    s.turns           = o["turns"].toInt();
    s.medianTurnS     = optional_double(o["turn_duration"].toObject()["median_s"]);
    s.pauses          = o["pauses"].toInt();
    s.pausesPerMin    = optional_double(o["pauses_per_min"]);
    s.backchannels    = o["backchannels"].toInt();
    s.medianResponseS = optional_double(o["response_offset"].toObject()["median_s"]);
    s.interruptions   = o["interruptions"].toInt();
    s.wordsPerMin     = optional_double(o["words_per_min"]);
    return s;
}

} // namespace

ConversationResult ConversationResult::load(const QString& jsonPath) {
    ConversationResult r;
    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return r;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root["schema"].toString() != QLatin1String(kSchema)) {
        return r;
    }

    r.sourceVideo_       = root["source_video"].toString();
    r.sourceAudio_       = root["source_audio"].toString();
    r.cameraIndex_       = root["camera_index"].toInt();
    r.videoStartMs_      = root["video_start_ms"].toDouble();
    r.annotatedVideo_    = root["annotated_video"].toString();
    r.timingMethod_      = root["timing"].toObject()["method"].toString();
    r.attributionMethod_ = root["attribution"].toObject()["method"].toString();

    for (const auto& v : root["turns"].toArray()) {
        const QJsonObject o = v.toObject();
        r.turns_ << ConversationTurn{o["speaker"].toString(), o["start_ms"].toInteger(),
                                     o["end_ms"].toInteger(), o["pauses"].toInt(),
                                     o["text"].toString()};
    }
    for (const auto& v : root["transitions"].toArray()) {
        const QJsonObject o = v.toObject();
        r.transitions_ << ConversationTransition{
            o["from"].toString(),           o["to"].toString(),  o["prev_end_ms"].toInteger(),
            o["next_start_ms"].toInteger(), o["fto_ms"].toInt(), o["interruption"].toBool()};
    }
    for (const auto& v : root["spurts"].toArray()) {
        const QJsonObject o = v.toObject();
        r.spurts_ << ConversationSpurt{o["speaker"].toString(), o["start_ms"].toInteger(),
                                       o["end_ms"].toInteger(), o["kind"].toString()};
    }
    std::sort(r.turns_.begin(), r.turns_.end(),
              [](const auto& a, const auto& b) { return a.startMs < b.startMs; });
    std::sort(r.transitions_.begin(), r.transitions_.end(),
              [](const auto& a, const auto& b) { return a.nextStartMs < b.nextStartMs; });
    std::sort(r.spurts_.begin(), r.spurts_.end(),
              [](const auto& a, const auto& b) { return a.startMs < b.startMs; });

    const QJsonObject summary  = root["summary"].toObject();
    const QJsonObject speakers = summary["speakers"].toObject();
    r.subject_                 = speaker_stats(speakers["subject"].toObject());
    r.other_                   = speaker_stats(speakers["other"].toObject());
    const QJsonObject tr       = summary["transitions"].toObject();
    r.transitionCount_         = tr["count"].toInt();
    r.medianFtoS_    = optional_double(tr["floor_transfer_offset"].toObject()["median_s"]);
    r.gaps_          = tr["gaps"].toInt();
    r.overlapping_   = tr["overlapping"].toInt();
    r.overlapTotalS_ = summary["overlap"].toObject()["total_s"].toDouble();
    r.silencePct_    = optional_double(summary["silence_pct"]);
    r.faceSeenPct_   = summary["coverage"].toObject()["face_seen_pct"].toDouble();

    const QJsonObject sig = root["signals"].toObject();
    r.signalStartMs_      = sig["start_ms"].toDouble();
    r.signalStepMs_       = sig["step_ms"].toDouble();
    r.audioDb_            = number_array(sig["audio_db"]);
    r.mouthActivity_      = number_array(sig["mouth_activity"]);

    r.valid_ = true;
    return r;
}

} // namespace mosaic
