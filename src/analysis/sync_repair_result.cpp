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
    result.alignment_  = root["alignment"].toString();

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
        cam.alignment            = camObj["alignment"].toString();
        cam.missingFrameCount    = camObj["missing_frame_count"].toInt(-1);
        cam.gapCount             = camObj["gap_count"].toInt(0);
        for (const auto& g : camObj["gaps"].toArray()) {
            const QJsonArray pair = g.toArray();
            if (pair.size() == 2) {
                cam.gaps.append({pair[0].toInt(), pair[1].toInt()});
            }
        }
        // null (arrival-time alignment) and absent both read as -1.
        cam.leadInTrimmed      = camObj["lead_in_trimmed"].toInt(-1);
        cam.tailTrimmed        = camObj["tail_trimmed"].toInt(-1);
        cam.alignmentUncertain = camObj["alignment_uncertain"].toBool(false);

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
