#include "analysis/pose_analysis_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "analysis/nearest_by_key.hpp"

namespace mosaic {

PoseAnalysisResult PoseAnalysisResult::load(const QString& jsonPath) {
    PoseAnalysisResult result;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.isEmpty()) {
        return result;
    }

    result.sourceVideo_ = root["source_video"].toString();
    result.model_       = root["model"].toString(); // absent -> "" (older files)

    for (const auto& kp : root["keypoint_names"].toArray()) {
        result.keypointNames_ << kp.toString();
    }

    for (const auto& edge : root["skeleton_edges"].toArray()) {
        const QJsonArray pair = edge.toArray();
        if (pair.size() == 2) {
            result.skeletonEdges_ << qMakePair(pair[0].toInt(), pair[1].toInt());
        }
    }

    for (const auto& frameVal : root["frames"].toArray()) {
        const QJsonObject frameObj = frameVal.toObject();

        PoseFrame frame;
        frame.frameIndex  = frameObj["frame_index"].toInt();
        frame.timestampNs = static_cast<int64_t>(frameObj["timestamp_ns"].toDouble());
        frame.cameraIndex = frameObj["camera_index"].toInt();

        for (const auto& subjVal : frameObj["subjects"].toArray()) {
            const QJsonObject subjObj = subjVal.toObject();

            PoseSubject subject;
            subject.subjectId  = subjObj["subject_id"].toInt();
            subject.confidence = subjObj["confidence"].toDouble();

            const QJsonArray kps = subjObj["keypoints"].toArray();
            for (const auto& kp : kps) {
                const QJsonArray xy = kp.toArray();
                subject.keypoints << (xy.size() == 2 ? QPointF(xy[0].toDouble(), xy[1].toDouble())
                                                     : QPointF());
            }

            for (const auto& vis : subjObj["visibilities"].toArray()) {
                subject.visibilities << vis.toDouble();
            }

            const QJsonArray bbox = subjObj["bbox_xyxy"].toArray();
            if (bbox.size() == 4) {
                subject.bbox = QRectF(QPointF(bbox[0].toDouble(), bbox[1].toDouble()),
                                      QPointF(bbox[2].toDouble(), bbox[3].toDouble()));
            }

            frame.subjects << subject;
        }

        result.frames_ << frame;
    }

    result.valid_ = true;
    return result;
}

const PoseFrame* PoseAnalysisResult::nearest_frame(int frameIndexEstimate) const {
    return nearest_by_key(frames_, frameIndexEstimate,
                          [](const PoseFrame& f) { return f.frameIndex; });
}

} // namespace mosaic
