#include "analysis/sync_repair_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace mosaic {

SyncRepairResult SyncRepairResult::load(const QString& jsonPath) {
    SyncRepairResult result;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.isEmpty()) {
        return result;
    }

    result.masterFps_  = root["master_fps"].toDouble();
    result.totalTicks_ = root["total_ticks"].toInt();
    result.durationMs_ = static_cast<int64_t>(root["duration_ms"].toDouble());

    for (const auto& camVal : root["cameras"].toArray()) {
        const QJsonObject camObj = camVal.toObject();

        SyncRepairCamera cam;
        cam.index                = camObj["index"].toInt();
        cam.sourceVideo          = camObj["source_video"].toString();
        cam.repairedVideo        = camObj["repaired_video"].toString();
        cam.sourceFramesCaptured = camObj["source_frames_captured"].toInt();
        cam.outputFrameCount     = camObj["output_frame_count"].toInt();
        cam.duplicatedFrameCount = camObj["duplicated_frame_count"].toInt();
        cam.skipped              = camObj["skipped"].toBool();
        cam.skipReason           = camObj["skip_reason"].toString();
        cam.note                 = camObj["note"].toString();

        result.cameras_ << cam;
    }

    result.valid_ = true;
    return result;
}

int SyncRepairResult::total_duplicated_frames() const {
    int total = 0;
    for (const auto& cam : cameras_) {
        if (!cam.skipped) {
            total += cam.duplicatedFrameCount;
        }
    }
    return total;
}

int SyncRepairResult::skipped_camera_count() const {
    int count = 0;
    for (const auto& cam : cameras_) {
        if (cam.skipped) {
            ++count;
        }
    }
    return count;
}

} // namespace mosaic
