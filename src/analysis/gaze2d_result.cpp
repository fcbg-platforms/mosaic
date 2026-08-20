#include "analysis/gaze2d_result.hpp"

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

} // namespace

Gaze2dResult Gaze2dResult::load(const QString& jsonPath) {
    Gaze2dResult result;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.isEmpty()) {
        return result;
    }

    result.sourceVideo_   = root["source_video"].toString();
    result.minConfidence_ = root["min_confidence"].toDouble();

    for (const auto& frameVal : root["frames"].toArray()) {
        const QJsonObject frameObj = frameVal.toObject();

        Gaze2dFrame frame;
        frame.frameIndex   = frameObj["frame_index"].toInt();
        frame.timestampMs  = static_cast<int64_t>(frameObj["timestamp_ms"].toDouble());
        frame.faceDetected = frameObj["face_detected"].toBool();
        if (frame.faceDetected) {
            const QJsonArray bboxArr = frameObj["face_box_px"].toArray();
            if (bboxArr.size() == 4) {
                const int x1    = bboxArr[0].toInt();
                const int y1    = bboxArr[1].toInt();
                const int x2    = bboxArr[2].toInt();
                const int y2    = bboxArr[3].toInt();
                frame.faceBoxPx = QRect(x1, y1, x2 - x1, y2 - y1);
            }
            frame.gazeDx = frameObj["gaze_dx"].toDouble();
            frame.gazeDy = frameObj["gaze_dy"].toDouble();
        }

        result.frames_ << frame;
    }

    // Written in chronological order by run_gaze2d.py, but sort
    // defensively rather than trust an external writer's ordering — same
    // discipline every sibling result class already applies.
    std::sort(
        result.frames_.begin(), result.frames_.end(),
        [](const Gaze2dFrame& a, const Gaze2dFrame& b) { return a.timestampMs < b.timestampMs; });

    const QJsonObject summary = root["summary"].toObject();
    result.pctFramesWithFace_ = summary["pct_frames_with_face"].toDouble();
    result.meanGazeDx_        = optional_double(summary["mean_gaze_dx"]);
    result.meanGazeDy_        = optional_double(summary["mean_gaze_dy"]);
    result.pctOnTarget_       = optional_double(summary["pct_on_target"]);

    result.valid_ = true;
    return result;
}

const Gaze2dFrame* Gaze2dResult::nearest_frame(int64_t timestampMsEstimate) const {
    return nearest_by_key(frames_, timestampMsEstimate,
                          [](const Gaze2dFrame& f) { return f.timestampMs; });
}

} // namespace mosaic
