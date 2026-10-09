#include "analysis/face_dynamics_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <limits>

#include "analysis/nearest_by_key.hpp"

namespace mosaic {

namespace {

constexpr auto kSchema = "mosaic-face-dynamics-v1";

double number_or_nan(const QJsonValue& v) {
    return v.isDouble() ? v.toDouble() : std::numeric_limits<double>::quiet_NaN();
}

std::optional<double> optional_double(const QJsonValue& v) {
    return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
}

} // namespace

FaceDynamicsResult FaceDynamicsResult::load(const QString& jsonPath) {
    FaceDynamicsResult result;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root["schema"].toString() != QLatin1String(kSchema)) {
        return result;
    }

    result.sourceVideo_    = root["source_video"].toString();
    result.cameraIndex_    = root["camera_index"].toInt();
    result.fps_            = root["fps"].toDouble();
    result.annotatedVideo_ = root["annotated_video"].toString();

    for (const auto& value : root["frames"].toArray()) {
        const QJsonObject o = value.toObject();
        FaceDynamicsFrame f;
        f.frameIndex   = o["frame_index"].toInt();
        f.timestampMs  = o["timestamp_ms"].toInteger();
        f.faceDetected = o["face_detected"].toBool();
        if (f.faceDetected) {
            const QJsonArray box = o["face_box_px"].toArray();
            if (box.size() == 4) {
                const int x1 = box[0].toInt();
                const int y1 = box[1].toInt();
                f.faceBoxPx  = QRect(x1, y1, box[2].toInt() - x1, box[3].toInt() - y1);
            }
            f.opennessLeft  = number_or_nan(o["openness_left"]);
            f.opennessRight = number_or_nan(o["openness_right"]);
            f.smile         = number_or_nan(o["smile"]);
            f.browRaise     = number_or_nan(o["brow_raise"]);
            f.expressivity  = number_or_nan(o["expressivity"]);
            f.yaw           = number_or_nan(o["yaw"]);
            f.pitch         = number_or_nan(o["pitch"]);
            f.roll          = number_or_nan(o["roll"]);
            f.headSpeed     = number_or_nan(o["head_speed"]);
        }
        result.frames_ << f;
    }
    std::sort(result.frames_.begin(), result.frames_.end(),
              [](const FaceDynamicsFrame& a, const FaceDynamicsFrame& b) {
                  return a.timestampMs < b.timestampMs;
              });

    for (const auto& value : root["events"].toArray()) {
        const QJsonObject o = value.toObject();
        FaceDynamicsEvent e;
        e.kind       = o["kind"].toString();
        e.startMs    = o["start_ms"].toInteger();
        e.endMs      = o["end_ms"].toInteger();
        e.peakMs     = o["peak_ms"].toInteger();
        e.durationMs = o["duration_ms"].toDouble();
        e.peak       = o["peak"].toDouble();
        result.events_ << e;
    }
    std::sort(result.events_.begin(), result.events_.end(),
              [](const FaceDynamicsEvent& a, const FaceDynamicsEvent& b) {
                  return a.startMs < b.startMs;
              });

    const QJsonObject s      = root["summary"].toObject();
    const QJsonObject blink  = s["blinks"].toObject();
    const QJsonObject expr   = s["expression"].toObject();
    const QJsonObject head   = s["head"].toObject();
    FaceDynamicsSummary& out = result.summary_;
    out.faceSeenS            = s["face_seen_s"].toDouble();
    out.faceSeenPct          = s["face_seen_pct"].toDouble();
    out.blinks               = blink["count"].toInt();
    out.blinksPerMinute      = optional_double(blink["per_minute"]);
    out.medianBlinkMs        = optional_double(blink["median_duration_ms"]);
    out.meanBlinkIntervalS   = optional_double(blink["mean_interval_s"]);
    out.longClosures         = blink["long_closures"].toInt();
    out.perclosPct           = optional_double(blink["perclos_pct"]);
    out.smiles               = expr["smiles"].toInt();
    out.duchenneSmiles       = expr["duchenne_smiles"].toInt();
    out.smilingPct           = optional_double(expr["smiling_pct"]);
    out.browRaises           = expr["brow_raises"].toInt();
    out.browFlashes          = expr["brow_flashes"].toInt();
    out.expressivityMean     = optional_double(expr["expressivity_mean"]);
    out.nods                 = head["nods"].toInt();
    out.shakes               = head["shakes"].toInt();
    out.meanHeadSpeed        = optional_double(head["mean_speed_deg_s"]);

    result.valid_ = true;
    return result;
}

const FaceDynamicsFrame* FaceDynamicsResult::nearest_frame(int64_t timestampMs) const {
    return nearest_by_key(frames_, timestampMs,
                          [](const FaceDynamicsFrame& f) { return f.timestampMs; });
}

} // namespace mosaic
