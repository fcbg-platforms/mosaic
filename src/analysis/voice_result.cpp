#include "analysis/voice_result.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

namespace mosaic {

namespace {

/// The only row order this build knows how to draw: row 0 is the *highest*
/// frequency, matching how the PNG is written (run_voice.py flips before
/// saving, so the C++ never has to).
///
/// Validated rather than assumed because getting it wrong is invisible. An
/// upside-down spectrogram still has formants, still has silence in the right
/// places, and still tracks the audio — it is simply mirrored, which nobody
/// notices for a week. Refusing an unknown value costs nothing and removes the
/// possibility entirely.
constexpr auto k_row_order_high_to_low = "high_to_low";

VoiceTrack parse_track(const QJsonObject& obj, const QString& valuesKey) {
    VoiceTrack track;
    if (obj.isEmpty()) {
        return track;
    }
    track.t0Ms = obj.value("t0_ms").toDouble();
    track.dtMs = obj.value("dt_ms").toDouble();

    const QJsonArray arr = obj.value(valuesKey).toArray();
    track.values.reserve(arr.size());
    for (const auto& v : arr) {
        track.values.append(static_cast<float>(v.toDouble()));
    }
    return track;
}

} // namespace

float VoiceTrack::at(double ms, float fallback) const {
    if (!is_valid()) {
        return fallback;
    }
    const double idx = (ms - t0Ms) / dtMs;
    if (idx < -0.5) {
        return fallback;
    }
    const auto i = static_cast<qsizetype>(std::llround(idx));
    if (i < 0 || i >= values.size()) {
        return fallback;
    }
    return values[i];
}

VoiceResult VoiceResult::load(const QString& jsonPath) {
    VoiceResult result;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.isEmpty()) {
        return result;
    }
    if (root.value("schema_version").toInt(-1) != k_voice_schema_version) {
        return result;
    }

    result.sourceAudio_ = root.value("source_audio").toString();
    result.durationMs_  = root.value("duration_ms").toDouble();

    const QJsonObject spec = root.value("spectrogram").toObject();
    if (!spec.isEmpty()) {
        if (spec.value("row_order").toString() != QLatin1String(k_row_order_high_to_low)) {
            return result; // see k_row_order_high_to_low
        }
        auto& meta  = result.spectrogram_;
        meta.width  = spec.value("width").toInt();
        meta.height = spec.value("height").toInt();
        meta.t0Ms   = spec.value("t0_ms").toDouble();
        meta.t1Ms   = spec.value("t1_ms").toDouble();
        meta.f0Hz   = spec.value("f0_hz").toDouble();
        meta.f1Hz   = spec.value("f1_hz").toDouble();
        meta.dbMin  = spec.value("db_min").toDouble();
        meta.dbMax  = spec.value("db_max").toDouble();

        // The image name comes out of the file, so it is untrusted input that
        // becomes a path. Accept a plain sibling filename only: anything
        // absolute, or containing a traversal segment, is dropped rather than
        // resolved. The result still loads — tracks are drawn without an
        // image — because refusing the whole file over a bad image reference
        // would lose data that is perfectly good.
        const QString image = spec.value("image").toString();
        const bool safe     = !image.isEmpty() && !QFileInfo(image).isAbsolute() &&
                          !image.contains("..") && !image.contains('/') && !image.contains('\\');
        if (safe) {
            meta.imagePath = QFileInfo(jsonPath).dir().filePath(image);
        }
    }

    const QJsonObject pitch = root.value("pitch").toObject();
    result.pitch_           = parse_track(pitch, "values_hz");
    result.pitchFloorHz_    = pitch.value("floor_hz").toDouble();
    result.pitchCeilingHz_  = pitch.value("ceiling_hz").toDouble();

    result.intensity_ = parse_track(root.value("intensity").toObject(), "values_db");

    // Valid even with no tracks and no image: a very short or silent clip
    // legitimately yields both, and the caller should show an empty panel
    // rather than an error.
    result.valid_ = true;
    return result;
}

} // namespace mosaic
