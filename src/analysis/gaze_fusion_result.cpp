#include "analysis/gaze_fusion_result.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

namespace mosaic {

namespace {

Vec3 vec3_from_json(const QJsonArray& arr, const Vec3& def = {0, 0, 0}) {
    if (arr.size() != 3) { return def; }
    return {arr[0].toDouble(), arr[1].toDouble(), arr[2].toDouble()};
}

} // namespace

GazeFusionResult GazeFusionResult::load(const QString& jsonPath) {
    GazeFusionResult result;

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
        GazeFusionRoomCamera cam;
        cam.index        = camObj["index"].toInt();
        cam.positionRoom = vec3_from_json(camObj["position_room"].toArray());
        result.cameras_ << cam;
    }

    const QJsonObject plane = root["plane"].toObject();
    result.planeDefined_ = plane["defined"].toBool();
    result.planePoint_   = vec3_from_json(plane["point"].toArray());
    result.planeNormal_  = vec3_from_json(plane["normal"].toArray(), Vec3{0, 0, 1});

    result.masterFps_ = root["master_fps"].toDouble(25.0);

    for (const auto& frameVal : root["frames"].toArray()) {
        const QJsonObject frameObj = frameVal.toObject();

        GazeFusionFrame frame;
        frame.tick           = static_cast<int64_t>(frameObj["tick"].toDouble());
        frame.timestampNs    = static_cast<int64_t>(frameObj["timestamp_ns"].toDouble());
        frame.numCameras     = frameObj["num_cameras"].toInt();
        frame.isTriangulated = frameObj["is_triangulated"].toBool();

        if (frameObj.contains("fused_origin_room")) {
            frame.fusedOriginRoom = vec3_from_json(frameObj["fused_origin_room"].toArray());
        }
        if (frameObj.contains("fused_direction_room")) {
            frame.fusedDirectionRoom = vec3_from_json(frameObj["fused_direction_room"].toArray());
        }
        frame.residualRmsMm =
            (frameObj.contains("residual_rms_mm") && !frameObj["residual_rms_mm"].isNull())
                ? frameObj["residual_rms_mm"].toDouble()
                : -1.0;

        if (frameObj.contains("target_point_room")) {
            frame.hasTarget       = true;
            frame.targetPointRoom = vec3_from_json(frameObj["target_point_room"].toArray());
        }

        for (const auto& camVal : frameObj["per_camera"].toArray()) {
            const QJsonObject camObj = camVal.toObject();

            GazeFusionCamera cam;
            cam.cameraIndex = camObj["camera_index"].toInt();

            const QJsonArray bbox = camObj["face_box_px"].toArray();
            if (bbox.size() == 4) {
                cam.faceBoxPx = QRectF(QPointF(bbox[0].toDouble(), bbox[1].toDouble()),
                                        QPointF(bbox[2].toDouble(), bbox[3].toDouble()));
            }
            cam.gazeDx        = camObj["gaze_dx"].toDouble();
            cam.gazeDy        = camObj["gaze_dy"].toDouble();
            cam.originRoom    = vec3_from_json(camObj["origin_room"].toArray());
            cam.directionRoom = vec3_from_json(camObj["direction_room"].toArray());
            cam.confidence    = camObj["confidence"].toDouble();

            frame.perCamera << cam;
        }

        result.frames_ << frame;
    }

    result.valid_ = true;
    return result;
}

const GazeFusionFrame* GazeFusionResult::nearest_frame(int64_t timestampNsEstimate) const {
    if (frames_.isEmpty()) {
        return nullptr;
    }

    const auto it = std::lower_bound(
        frames_.begin(), frames_.end(), timestampNsEstimate,
        [](const GazeFusionFrame& f, int64_t ts) { return f.timestampNs < ts; });

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
