#include "analysis/pose_analysis_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>

#include "analysis/nearest_by_key.hpp"

namespace mosaic {

namespace {

/// The distinct subject ids that may be followed *across* frames, ascending.
///
/// Negative ids are deliberately excluded. run_pose.py assigns them as
/// -(detection index + 1) for detections its tracker never claimed, which
/// makes them frame-local: the "-1" in one frame and the "-1" in the next are
/// two unrelated people. Aggregating them would splice those people into one
/// trajectory and manufacture exactly the fabricated speed spikes this whole
/// identity change exists to remove — so anything that reasons over time
/// (chips, chart, kinematics, the CSV export) must not see them. They are
/// still drawn on the video overlay, where each frame stands alone and no
/// cross-frame claim is made.
///
/// Ordering is by id rather than by prominence (frame count, say). A
/// pre-tracking file's ids are dense array positions with near-identical frame
/// counts, so a prominence order would shuffle those chips into an
/// arbitrary-but-different arrangement; id order reproduces the old layout
/// exactly, and is stable across re-analysis of the same footage.
QVector<SubjectId> collect_subject_ids(const QVector<PoseFrame>& frames) {
    QSet<int> seen;
    for (const auto& frame : frames) {
        for (const auto& subject : frame.subjects) {
            if (subject.subjectId >= 0) {
                seen.insert(subject.subjectId);
            }
        }
    }

    QVector<int> ids(seen.cbegin(), seen.cend());
    std::sort(ids.begin(), ids.end());

    QVector<SubjectId> out;
    out.reserve(ids.size());
    for (const int id : ids) {
        out << SubjectId(id);
    }
    return out;
}

/// Whether any frame holds a detection the tracker never claimed. Surfaced so
/// the UI can say those detections exist rather than silently omitting them
/// from the chips and the export.
bool any_untracked(const QVector<PoseFrame>& frames) {
    for (const auto& frame : frames) {
        for (const auto& subject : frame.subjects) {
            if (subject.subjectId < 0) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

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
    result.model_       = root["model"].toString();   // absent -> "" (older files)
    result.tracker_     = root["tracker"].toString(); // absent -> "" (pre-tracking files)

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
            // An absent "subject_id" falls back to this subject's position in
            // the frame's array — exactly the convention run_pose.py used
            // before tracking existed. Deliberately NOT QJsonValue::toInt()'s
            // own 0 default: that collapses every subject in the frame onto id
            // 0, and an identity-keyed lookup would then quietly return
            // whichever person happened to be listed first, rather than
            // failing. Only reachable for hand-edited or truncated files.
            const QJsonValue idVal = subjObj.value("subject_id");
            subject.subjectId =
                idVal.isDouble() ? idVal.toInt() : static_cast<int>(frame.subjects.size());
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

    result.subjectIds_             = collect_subject_ids(result.frames_);
    result.hasUntrackedDetections_ = any_untracked(result.frames_);
    result.valid_                  = true;
    return result;
}

const PoseFrame* PoseAnalysisResult::nearest_frame(int frameIndexEstimate) const {
    return nearest_by_key(frames_, frameIndexEstimate,
                          [](const PoseFrame& f) { return f.frameIndex; });
}

} // namespace mosaic
