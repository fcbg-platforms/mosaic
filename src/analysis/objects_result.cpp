#include "analysis/objects_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace mosaic {

ObjectsResult ObjectsResult::load(const QString& path) {
    ObjectsResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return r;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root["schema"].toString() != QLatin1String("mosaic-objects-v1")) {
        return r;
    }
    for (const auto& v : root["labels"].toArray()) {
        r.labels_ << v.toString();
    }
    r.videoStartMs_   = root["video_start_ms"].toDouble();
    r.poseUsed_       = root["pose_used"].toBool();
    r.handsFollowed_  = root["hands_followed"].toBool();
    r.annotatedVideo_ = root["annotated_video"].toString();
    for (const auto& v : root["summary"].toObject()["objects"].toArray()) {
        const QJsonObject o = v.toObject();
        Object x;
        x.id                 = o["id"].toInt();
        x.label              = o["label"].toString();
        x.seenPct            = o["seen_pct"].toDouble();
        x.travelPx           = o["travel_px"].toDouble();
        x.contacts           = o["contacts"].toInt();
        const QJsonObject by = o["touched_s_by_person"].toObject();
        for (auto it = by.begin(); it != by.end(); ++it) {
            x.touchedSByPerson.insert(it.key(), it.value().toDouble());
        }
        for (const auto& span : o["seen_spans"].toArray()) {
            const QJsonArray a = span.toArray();
            if (a.size() == 2) {
                x.seenSpans.append({a[0].toDouble(), a[1].toDouble()});
            }
        }
        r.objects_ << x;
    }
    for (const auto& v : root["contacts"].toArray()) {
        const QJsonObject o = v.toObject();
        // The person is a Pose subject id: a number, or null for a hand Pose
        // did not place (or a person it did not track).
        r.contacts_ << Contact{
            o["object_id"].toInt(),
            o["person"].isDouble() ? QString::number(o["person"].toInt()) : QStringLiteral("?"),
            o["start_ms"].toDouble(), o["end_ms"].toDouble()};
    }
    r.valid_ = true;
    return r;
}

} // namespace mosaic
