#include "analysis/expression_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "analysis/nearest_by_key.hpp"

namespace mosaic {

ExpressionResult ExpressionResult::load(const QString& jsonPath) {
    ExpressionResult result;

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

    for (const auto& name : root["blendshape_names"].toArray()) {
        result.blendshapeNames_ << name.toString();
    }

    for (const auto& name : root["au_names"].toArray()) {
        result.auNames_ << name.toString();
    }

    for (const auto& frameVal : root["frames"].toArray()) {
        const QJsonObject frameObj = frameVal.toObject();

        ExpressionFrame frame;
        frame.frameIndex  = frameObj["frame_index"].toInt();
        frame.timestampNs = static_cast<int64_t>(frameObj["timestamp_ns"].toDouble());
        frame.cameraIndex = frameObj["camera_index"].toInt();

        for (const auto& subjVal : frameObj["subjects"].toArray()) {
            const QJsonObject subjObj = subjVal.toObject();

            ExpressionSubject subject;
            subject.subjectId  = subjObj["subject_id"].toInt();
            subject.confidence = subjObj["confidence"].toDouble();

            const QJsonArray bbox = subjObj["bbox_xyxy"].toArray();
            if (bbox.size() == 4) {
                subject.bbox = QRectF(QPointF(bbox[0].toDouble(), bbox[1].toDouble()),
                                      QPointF(bbox[2].toDouble(), bbox[3].toDouble()));
            }

            for (const auto& score : subjObj["blendshape_scores"].toArray()) {
                subject.blendshapeScores << score.toDouble();
            }

            subject.dominantExpression = subjObj["dominant_expression"].toString();
            subject.dominantScore      = subjObj["dominant_score"].toDouble();

            // Name-keyed on the wire (matches the plan's stated schema),
            // built into a parallel array ordered by result.auNames_ here —
            // same "always look up by name, never trust positional order"
            // discipline BLENDSHAPE_NAMES lookups already rely on. A
            // missing/unrecognized name defaults to 0.0 via
            // QJsonValue::Undefined.toDouble(), and this loop is simply
            // empty (leaving actionUnits empty) for any file with no
            // "au_names" at all — no special-casing needed for backward
            // compatibility.
            const QJsonObject auObj = subjObj["action_units"].toObject();
            for (const auto& auName : result.auNames_) {
                subject.actionUnits << auObj[auName].toDouble();
            }

            frame.subjects << subject;
        }

        result.frames_ << frame;
    }

    result.valid_ = true;
    return result;
}

const ExpressionFrame* ExpressionResult::nearest_frame(int frameIndexEstimate) const {
    return nearest_by_key(frames_, frameIndexEstimate,
                          [](const ExpressionFrame& f) { return f.frameIndex; });
}

} // namespace mosaic
