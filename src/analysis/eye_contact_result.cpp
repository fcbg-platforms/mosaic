#include "analysis/eye_contact_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <limits>

namespace mosaic {

namespace {

constexpr auto kSchema = "mosaic-eye-contact-v1";

double number_or_nan(const QJsonValue& v) {
    return v.isDouble() ? v.toDouble() : std::numeric_limits<double>::quiet_NaN();
}

std::optional<double> optional_double(const QJsonValue& v) {
    return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
}

} // namespace

EyeContactResult EyeContactResult::load(const QString& jsonPath) {
    EyeContactResult r;
    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return r;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root["schema"].toString() != QLatin1String(kSchema)) {
        return r;
    }
    r.sourceVideo_        = root["source_video"].toString();
    r.annotatedVideo_     = root["annotated_video"].toString();
    const QJsonObject tgt = root["target"].toObject();
    r.targetMethod_       = tgt["method"].toString();
    r.targetYaw_          = optional_double(tgt["yaw"]);
    r.targetPitch_        = optional_double(tgt["pitch"]);
    r.radiusDeg_          = optional_double(tgt["radius_deg"]);

    for (const auto& v : root["frames"].toArray()) {
        const QJsonObject o = v.toObject();
        EyeContactFrame f;
        f.frameIndex       = o["frame_index"].toInt();
        f.timestampMs      = o["timestamp_ms"].toInteger();
        f.faceDetected     = o["face_detected"].toBool();
        f.gazeYaw          = number_or_nan(o["gaze_yaw"]);
        f.gazePitch        = number_or_nan(o["gaze_pitch"]);
        f.offsetDeg        = number_or_nan(o["offset_deg"]);
        const QJsonValue c = o["contact"];
        f.contact          = c.isBool() ? (c.toBool() ? 1 : 0) : -1;
        r.frames_ << f;
    }
    std::sort(r.frames_.begin(), r.frames_.end(),
              [](const auto& a, const auto& b) { return a.timestampMs < b.timestampMs; });

    for (const auto& v : root["aversions"].toArray()) {
        const QJsonObject o = v.toObject();
        r.aversions_ << EyeContactAversion{o["start_ms"].toInteger(), o["end_ms"].toInteger(),
                                           o["duration_ms"].toInt(), o["offset_deg"].toDouble(),
                                           o["direction"].toString()};
    }
    std::sort(r.aversions_.begin(), r.aversions_.end(),
              [](const auto& a, const auto& b) { return a.startMs < b.startMs; });

    const QJsonObject s      = root["summary"].toObject();
    EyeContactSummary& out   = r.summary_;
    out.eyeContactPct        = optional_double(s["eye_contact_pct"]);
    out.listeningPct         = optional_double(s["eye_contact_listening_pct"]);
    out.speakingPct          = optional_double(s["eye_contact_speaking_pct"]);
    out.aversions            = s["aversions"].toInt();
    out.aversionsPerMin      = optional_double(s["aversions_per_min"]);
    out.aversionMedianS      = optional_double(s["aversion_median_s"]);
    const QJsonObject dirs   = s["aversion_directions"].toObject();
    out.up                   = dirs["up"].toInt();
    out.down                 = dirs["down"].toInt();
    out.left                 = dirs["left"].toInt();
    out.right                = dirs["right"].toInt();
    const QJsonObject t      = s["turns"].toObject();
    out.subjectTurns         = t["subject_turns"].toInt();
    out.turnStartAversionPct = optional_double(t["turn_start_aversion_pct"]);
    out.turnEndContactPct    = optional_double(t["turn_end_contact_pct"]);
    out.gapAversionPct       = optional_double(t["gap_aversion_pct"]);

    r.valid_ = true;
    return r;
}

} // namespace mosaic
