#include "analysis/hands_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace mosaic {

HandsResult HandsResult::load(const QString& path) {
    HandsResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return r;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root["schema"].toString() != QLatin1String("mosaic-hands-v1")) {
        return r;
    }
    r.model_          = root["model"].toString();
    r.stride_         = root["stride"].toInt(1);
    r.videoStartMs_   = root["video_start_ms"].toDouble();
    r.annotatedVideo_ = root["annotated_video"].toString();
    for (const auto& v : root["hands"].toArray()) {
        const QJsonObject o = v.toObject();
        Hand h;
        // null for a hand no person's wrist was near (MediaPipe).
        h.subjectId    = o["subject_id"].isDouble() ? o["subject_id"].toInt() : -1;
        h.side         = o["side"].toString();
        h.seenMs       = o["seen_ms"].toDouble();
        h.medianSizePx = o["median_size_px"].toDouble();
        h.meanScore    = o["mean_score"].toDouble();
        for (const auto& span : o["seen_spans"].toArray()) {
            const QJsonArray a = span.toArray();
            if (a.size() == 2) {
                h.seenSpans.append({a[0].toDouble(), a[1].toDouble()});
            }
        }
        r.hands_ << h;
    }
    r.valid_ = true;
    return r;
}

} // namespace mosaic
