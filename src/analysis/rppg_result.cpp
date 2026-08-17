#include "analysis/rppg_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

#include "analysis/nearest_by_key.hpp"

namespace mosaic {

namespace {

std::optional<double> optional_double(const QJsonValue& v) {
    return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
}

double double_or_nan(const QJsonValue& v) {
    return v.isDouble() ? v.toDouble() : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

RppgResult RppgResult::load(const QString& jsonPath) {
    RppgResult result;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.isEmpty()) {
        return result;
    }

    result.sourceVideo_ = root["source_video"].toString();
    result.backend_     = root["backend"].toString();
    result.windowSec_   = root["window_sec"].toDouble();
    result.hopSec_      = root["hop_sec"].toDouble();

    for (const auto& winVal : root["windows"].toArray()) {
        const QJsonObject winObj = winVal.toObject();

        RppgWindow win;
        win.startMs            = static_cast<int64_t>(winObj["start_ms"].toDouble());
        win.endMs              = static_cast<int64_t>(winObj["end_ms"].toDouble());
        win.bpm                = double_or_nan(winObj["bpm"]);
        win.smoothedBpm        = double_or_nan(winObj["smoothed_bpm"]);
        win.snrDb              = double_or_nan(winObj["snr_db"]);
        win.validFrameFraction = winObj["valid_frame_fraction"].toDouble();

        result.windows_ << win;
    }

    for (const auto& frameVal : root["frames"].toArray()) {
        const QJsonObject frameObj = frameVal.toObject();

        RppgFrame frame;
        frame.frameIndex   = frameObj["frame_index"].toInt();
        frame.timestampMs  = static_cast<int64_t>(frameObj["timestamp_ms"].toDouble());
        frame.faceDetected = frameObj["face_detected"].toBool();
        if (frame.faceDetected) {
            const QJsonArray bboxArr = frameObj["roi_bbox_px"].toArray();
            if (bboxArr.size() == 4) {
                frame.roiBboxPx = QRect(bboxArr[0].toInt(), bboxArr[1].toInt(), bboxArr[2].toInt(),
                                        bboxArr[3].toInt());
            }
        }

        result.frames_ << frame;
    }

    // Both arrays are written in chronological order by run_rppg.py, but
    // sort defensively rather than trust an external writer's ordering —
    // same discipline TranscriptResult::load() already applies to
    // faster-whisper's occasionally-non-monotonic segment timestamps.
    std::sort(result.windows_.begin(), result.windows_.end(),
              [](const RppgWindow& a, const RppgWindow& b) { return a.startMs < b.startMs; });
    std::sort(result.frames_.begin(), result.frames_.end(),
              [](const RppgFrame& a, const RppgFrame& b) { return a.timestampMs < b.timestampMs; });

    const QJsonObject summary = root["summary"].toObject();
    result.meanBpm_           = optional_double(summary["mean_bpm"]);
    result.medianBpm_         = optional_double(summary["median_bpm"]);
    result.minBpm_            = optional_double(summary["min_bpm"]);
    result.maxBpm_            = optional_double(summary["max_bpm"]);
    result.pctWindowsGood_    = summary["pct_windows_good"].toDouble();

    result.valid_ = true;
    return result;
}

const RppgWindow* RppgResult::nearest_window(int64_t timestampMsEstimate) const {
    return nearest_by_key(windows_, timestampMsEstimate,
                          [](const RppgWindow& w) { return w.startMs; });
}

const RppgFrame* RppgResult::nearest_frame(int64_t timestampMsEstimate) const {
    return nearest_by_key(frames_, timestampMsEstimate,
                          [](const RppgFrame& f) { return f.timestampMs; });
}

} // namespace mosaic
