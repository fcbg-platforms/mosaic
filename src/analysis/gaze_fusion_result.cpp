#include "analysis/gaze_fusion_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>

#include "analysis/nearest_by_key.hpp"

namespace mosaic {

namespace {

Vec3 vec3_from_json(const QJsonValue& v, const Vec3& def = {0, 0, 0}) {
    const QJsonArray arr = v.toArray();
    if (arr.size() != 3) {
        return def;
    }
    return {arr[0].toDouble(), arr[1].toDouble(), arr[2].toDouble()};
}

bool is_vec3(const QJsonValue& v) { return v.isArray() && v.toArray().size() == 3; }

QRectF box_from_json(const QJsonValue& v) {
    const QJsonArray b = v.toArray();
    if (b.size() != 4) {
        return {};
    }
    return QRectF(QPointF(b[0].toDouble(), b[1].toDouble()),
                  QPointF(b[2].toDouble(), b[3].toDouble()));
}

// mosaic-gaze-fusion-v2: frames[].subjects[].
GazeSubjectSample subject_from_v2(const QJsonObject& o) {
    GazeSubjectSample s;
    s.id                     = o["id"].toString();
    s.name                   = o["name"].toString(s.id);
    s.origin                 = vec3_from_json(o["origin"]);
    s.hasDirection           = is_vec3(o["direction"]);
    s.direction              = vec3_from_json(o["direction"]);
    s.hasPoint               = is_vec3(o["point"]);
    s.point                  = vec3_from_json(o["point"]);
    const QJsonObject target = o["target"].toObject();
    s.targetType             = target["type"].toString();
    s.targetLabel            = target["label"].toString();
    s.targetSubject          = target["subject"].toString();
    s.mutual                 = o["mutual"].toBool();
    s.confidence             = o["confidence"].toDouble();
    s.uncertaintyDeg = o["uncertainty_deg"].isNull() ? -1.0 : o["uncertainty_deg"].toDouble(-1.0);
    s.numCameras     = o["n_cameras"].toInt();
    for (const auto& cv : o["per_camera"].toArray()) {
        const QJsonObject c = cv.toObject();
        GazeFusionCamera cam;
        cam.cameraIndex   = c["camera"].toInt(-1);
        cam.faceBoxPx     = box_from_json(c["face_box_px"]);
        cam.hasDirection  = is_vec3(c["direction"]);
        cam.directionRoom = vec3_from_json(c["direction"]);
        cam.weight        = c["weight"].toDouble();
        s.perCamera << cam;
    }
    return s;
}

// mosaic-gaze-fusion-v1: one implicit subject per frame, fused_* fields.
GazeSubjectSample subject_from_v1(const QJsonObject& f) {
    GazeSubjectSample s;
    s.id           = QStringLiteral("S1");
    s.name         = s.id;
    s.origin       = vec3_from_json(f["fused_origin_room"]);
    s.hasDirection = is_vec3(f["fused_direction_room"]);
    s.direction    = vec3_from_json(f["fused_direction_room"]);
    s.hasPoint     = is_vec3(f["target_point_room"]);
    s.point        = vec3_from_json(f["target_point_room"]);
    s.targetType   = s.hasPoint ? QStringLiteral("plane") : QString();
    s.targetLabel  = s.hasPoint ? QStringLiteral("plane") : QString();
    s.numCameras   = f["num_cameras"].toInt();
    for (const auto& cv : f["per_camera"].toArray()) {
        const QJsonObject c = cv.toObject();
        GazeFusionCamera cam;
        cam.cameraIndex   = c["camera_index"].toInt(-1);
        cam.faceBoxPx     = box_from_json(c["face_box_px"]);
        cam.hasDirection  = is_vec3(c["direction_room"]);
        cam.directionRoom = vec3_from_json(c["direction_room"]);
        cam.weight        = c["confidence"].toDouble();
        s.perCamera << cam;
    }
    return s;
}

} // namespace

const GazeSubjectSample* GazeFusionFrame::subject(const QString& id) const {
    for (const auto& s : subjects) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

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
    const bool v2         = root["schema"].toString() == QLatin1String("mosaic-gaze-fusion-v2");
    result.schemaVersion_ = v2 ? 2 : 1;

    if (v2) {
        const QJsonObject videos = root["source_videos"].toObject();
        for (auto it = videos.begin(); it != videos.end(); ++it) {
            result.sourceVideos_ << it.value().toString();
        }
        const QJsonObject annotated = root["annotated_videos"].toObject();
        for (auto it = annotated.begin(); it != annotated.end(); ++it) {
            result.annotatedVideos_.append({it.key().toInt(), it.value().toString()});
        }
        result.roomVideo_     = root["room_video"].toString();
        result.source_        = root["source"].toString();
        result.masterFps_     = root["fps"].toDouble(25.0);
        result.analysedEvery_ = std::max(1, root["analysed_every"].toInt(1));
        for (const auto& sv : root["subjects"].toArray()) {
            const QJsonObject s = sv.toObject();
            result.subjectIds_ << s["id"].toString();
            result.subjectNames_ << s["name"].toString(s["id"].toString());
        }
        for (const auto& rv : root["regions"].toArray()) {
            const QJsonObject r = rv.toObject();
            GazeFusionRegion region;
            region.name   = r["name"].toString();
            region.centre = vec3_from_json(r["centre"]);
            region.normal = vec3_from_json(r["normal"], Vec3{0, 0, 1});
            region.uAxis  = vec3_from_json(r["u_axis"], Vec3{1, 0, 0});
            region.width  = r["width"].toDouble();
            region.height = r["height"].toDouble();
            result.regions_ << region;
        }
    } else {
        for (const auto& v : root["source_videos"].toArray()) {
            result.sourceVideos_ << v.toString();
        }
        result.masterFps_ = root["master_fps"].toDouble(25.0);
    }

    for (const auto& camVal : root["cameras"].toArray()) {
        const QJsonObject camObj = camVal.toObject();
        GazeFusionRoomCamera cam;
        cam.index        = camObj["index"].toInt();
        cam.positionRoom = vec3_from_json(camObj["position_room"]);
        result.cameras_ << cam;
    }

    const QJsonObject plane = root["plane"].toObject();
    result.planeDefined_    = plane["defined"].toBool();
    result.planePoint_      = vec3_from_json(plane["point"]);
    result.planeNormal_     = vec3_from_json(plane["normal"], Vec3{0, 0, 1});

    bool anyV1Subject = false;
    for (const auto& frameVal : root["frames"].toArray()) {
        const QJsonObject f = frameVal.toObject();
        GazeFusionFrame frame;
        frame.tick        = f["tick"].toInteger();
        frame.timestampNs = f["timestamp_ns"].toInteger(); // exact: ns exceed 2^53
        if (v2) {
            frame.videoFrameIndex = f["video_frame_index"].toInteger(-1);
            for (const auto& sv : f["subjects"].toArray()) {
                frame.subjects << subject_from_v2(sv.toObject());
            }
        } else if (f["num_cameras"].toInt() > 0) {
            frame.subjects << subject_from_v1(f);
            anyV1Subject = true;
        }
        result.frames_ << frame;
    }
    if (!v2 && anyV1Subject) {
        result.subjectIds_ << QStringLiteral("S1");
        result.subjectNames_ << QStringLiteral("S1");
    }

    result.valid_ = true;
    return result;
}

QString GazeFusionResult::annotated_video(int cameraIndex) const {
    for (const auto& [index, path] : annotatedVideos_) {
        if (index == cameraIndex) {
            return path;
        }
    }
    return {};
}

QString GazeFusionResult::subject_name(const QString& id) const {
    const auto i = subjectIds_.indexOf(id);
    return i >= 0 && i < subjectNames_.size() ? subjectNames_[i] : id;
}

const GazeFusionFrame* GazeFusionResult::nearest_frame(int64_t timestampNsEstimate) const {
    return nearest_by_key(frames_, timestampNsEstimate,
                          [](const GazeFusionFrame& f) { return f.timestampNs; });
}

const GazeFusionFrame* GazeFusionResult::frame_at_position_ms(int64_t positionMs) const {
    if (frames_.isEmpty()) {
        return nullptr;
    }
    if (schemaVersion_ >= 2 && source_ != QLatin1String("raw")) {
        // The frame on screen at this position (floor, not round: a frame
        // stays shown until the next one starts).
        const auto index = static_cast<int64_t>(
            std::floor(static_cast<double>(positionMs) * masterFps_ / 1000.0 + 1e-6));
        const auto hold = std::max<int64_t>(analysedEvery_, std::llround(0.3 * masterFps_));
        return frame_for_video_frame(index, hold);
    }
    return nearest_frame(frames_.first().timestampNs + positionMs * 1000000LL);
}

const GazeFusionFrame* GazeFusionResult::frame_for_video_frame(int64_t videoFrameIndex,
                                                               int64_t holdFrames) const {
    if (schemaVersion_ < 2 || frames_.isEmpty()) {
        return nullptr;
    }
    // Frames are in ascending videoFrameIndex: the last one at or before it.
    auto it =
        std::upper_bound(frames_.begin(), frames_.end(), videoFrameIndex,
                         [](int64_t v, const GazeFusionFrame& f) { return v < f.videoFrameIndex; });
    if (it == frames_.begin()) {
        return nullptr;
    }
    --it;
    return videoFrameIndex - it->videoFrameIndex <= holdFrames ? &*it : nullptr;
}

} // namespace mosaic
