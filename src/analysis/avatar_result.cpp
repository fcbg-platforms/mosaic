#include "analysis/avatar_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <limits>

namespace mosaic {

namespace {

double number_or_nan(const QJsonValue& v) {
    return v.isDouble() ? v.toDouble() : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

AvatarResult AvatarResult::load(const QString& path) {
    AvatarResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return r;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root["schema"].toString() != QLatin1String("mosaic-avatar-v1")) {
        return r;
    }
    const QJsonArray rows = root["to_room"].toArray();
    if (rows.size() != 3) {
        return r;
    }
    for (int i = 0; i < 3; ++i) {
        const QJsonArray row = rows[i].toArray();
        for (int j = 0; j < 3; ++j) {
            r.toRoom_[static_cast<std::size_t>(3 * i + j)] = row.at(j).toDouble();
        }
    }
    for (const auto& v : root["people"].toArray()) {
        const QJsonObject o = v.toObject();
        Person p;
        p.personId              = o["person_id"].toInt(-1);
        p.file                  = o["file"].toString();
        p.vertices              = o["vertices"].toInt();
        p.faces                 = o["faces"].toInt();
        p.weightsPerVertex      = o["weights_per_vertex"].toInt();
        p.ticks                 = o["ticks"].toInt();
        p.boneCount             = o["bone_count"].toInt();
        p.bytes                 = static_cast<qint64>(o["bytes"].toDouble());
        p.seconds               = o["seconds"].toDouble();
        p.medianErrorMm         = number_or_nan(o["median_error_mm"]);
        p.p90ErrorMm            = number_or_nan(o["p90_error_mm"]);
        const QJsonObject pheno = o["phenotype"].toObject();
        for (auto it = pheno.begin(); it != pheno.end(); ++it) {
            p.phenotype.insert(it.key(), it.value().toDouble());
        }
        const QJsonObject joints = o["per_joint_median_error_mm"].toObject();
        for (auto it = joints.begin(); it != joints.end(); ++it) {
            p.perJointErrorMm.insert(it.key(), number_or_nan(it.value()));
        }
        const QJsonObject p90 = o["per_joint_p90_error_mm"].toObject();
        for (auto it = p90.begin(); it != p90.end(); ++it) {
            p.perJointP90ErrorMm.insert(it.key(), number_or_nan(it.value()));
        }
        r.people_ << p;
    }
    for (const auto& v : root["bone_labels"].toArray()) {
        r.boneLabels_ << v.toString();
    }
    for (const auto& v : root["failed"].toArray()) {
        r.failed_ << v.toInt();
    }
    r.valid_ = true;
    return r;
}

} // namespace mosaic
