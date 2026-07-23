#include "analysis/skeleton3d_result.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

namespace mosaic {

namespace {

Skeleton3DVec3 vec3_from_json(const QJsonArray& arr, const Skeleton3DVec3& def = {0, 0, 0}) {
    if (arr.size() != 3) { return def; }
    return {arr[0].toDouble(), arr[1].toDouble(), arr[2].toDouble()};
}

} // namespace

Skeleton3DResult Skeleton3DResult::load(const QString& jsonPath) {
    Skeleton3DResult result;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.isEmpty()) {
        return result;
    }

    for (const auto& v : root["source_videos"].toArray()) {
        result.sourceVideos_ << v.toString();
    }

    for (const auto& camVal : root["cameras"].toArray()) {
        const QJsonObject camObj = camVal.toObject();
        Skeleton3DRoomCamera cam;
        cam.index        = camObj["index"].toInt();
        cam.positionRoom = vec3_from_json(camObj["position_room"].toArray());
        result.cameras_ << cam;
    }

    for (const auto& kp : root["keypoint_names"].toArray()) {
        result.keypointNames_ << kp.toString();
    }

    for (const auto& edge : root["skeleton_edges"].toArray()) {
        const QJsonArray pair = edge.toArray();
        if (pair.size() == 2) {
            result.skeletonEdges_ << qMakePair(pair[0].toInt(), pair[1].toInt());
        }
    }

    result.masterFps_ = root["master_fps"].toDouble(25.0);

    for (const auto& frameVal : root["frames"].toArray()) {
        const QJsonObject frameObj = frameVal.toObject();

        Skeleton3DFrame frame;
        frame.tick        = static_cast<int64_t>(frameObj["tick"].toDouble());
        frame.timestampNs = static_cast<int64_t>(frameObj["timestamp_ns"].toDouble());

        for (const auto& personVal : frameObj["people"].toArray()) {
            const QJsonObject personObj = personVal.toObject();

            Skeleton3DPerson person;
            person.trackId = personObj["track_id"].toInt();
            person.numContributingCameras = personObj["num_contributing_cameras"].toInt();
            for (const auto& c : personObj["source_cameras"].toArray()) {
                person.sourceCameras << c.toInt();
            }

            const QJsonArray kpsRoom  = personObj["keypoints_room"].toArray();
            const QJsonArray kpsValid = personObj["keypoints_valid"].toArray();
            const QJsonArray kpsErr   = personObj["reprojection_error_px"].toArray();
            const int n = kpsRoom.size();

            for (int i = 0; i < n; ++i) {
                Skeleton3DKeypoint kp;
                kp.valid = (i < kpsValid.size()) && kpsValid[i].toBool();
                if (kp.valid) {
                    kp.positionRoom        = vec3_from_json(kpsRoom[i].toArray());
                    kp.reprojectionErrorPx = (i < kpsErr.size()) ? kpsErr[i].toDouble() : -1.0;
                }
                person.keypoints << kp;
            }

            const QJsonObject reprojObj = personObj["reprojected_px"].toObject();
            for (auto it = reprojObj.constBegin(); it != reprojObj.constEnd(); ++it) {
                bool ok = false;
                const int camIndex = it.key().toInt(&ok);
                if (!ok) { continue; }

                QVector<QPointF> pts;
                const QJsonArray ptsArr = it.value().toArray();
                pts.reserve(ptsArr.size());
                for (const auto& ptVal : ptsArr) {
                    const QJsonArray xy = ptVal.toArray();
                    pts << (xy.size() == 2
                        ? QPointF(xy[0].toDouble(), xy[1].toDouble())
                        : QPointF());
                }
                person.reprojectedPx.insert(camIndex, pts);
            }

            frame.people << person;
        }

        result.frames_ << frame;
    }

    result.valid_ = true;
    return result;
}

const Skeleton3DFrame* Skeleton3DResult::nearest_frame(int64_t timestampNsEstimate) const {
    if (frames_.isEmpty()) {
        return nullptr;
    }

    const auto it = std::lower_bound(
        frames_.begin(), frames_.end(), timestampNsEstimate,
        [](const Skeleton3DFrame& f, int64_t ts) { return f.timestampNs < ts; });

    if (it == frames_.begin()) {
        return &(*it);
    }
    if (it == frames_.end()) {
        return &(*std::prev(it));
    }

    const auto prevIt = std::prev(it);
    const int64_t afterDelta  = it->timestampNs - timestampNsEstimate;
    const int64_t beforeDelta = timestampNsEstimate - prevIt->timestampNs;
    return (beforeDelta <= afterDelta) ? &(*prevIt) : &(*it);
}

} // namespace mosaic
