#include "analysis/clap_check_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <cmath>

namespace mosaic {

ClapCheckResult ClapCheckResult::load(const QString& path) {
    ClapCheckResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return r;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root["schema"].toString() != QLatin1String("mosaic-clap-check-v1")) {
        return r;
    }
    r.verdict_ = root["verdict"].toString();
    for (const auto& c : root["cameras"].toArray()) {
        const QJsonObject cam = c.toObject();
        for (const auto& m : cam["microphones"].toArray()) {
            const QJsonObject mic = m.toObject();
            Pair p;
            p.camera    = cam["camera"].toInt(-1);
            p.frameMs   = cam["frame_ms"].toDouble(40.0);
            p.audioFile = mic["file"].toString();
            p.matched   = mic["matched"].toInt();
            if (mic["median_offset_ms"].isDouble()) {
                p.medianOffsetMs = mic["median_offset_ms"].toDouble();
            }
            if (mic["spread_ms"].isDouble()) {
                p.spreadMs = mic["spread_ms"].toDouble();
            }
            p.verdict = mic["verdict"].toString();
            r.pairs_ << p;
        }
    }
    r.valid_ = true;
    return r;
}

QString ClapCheckResult::summary() const {
    QStringList lines;
    for (const auto& p : pairs_) {
        const QString who = QString("Camera %1 / %2").arg(p.camera + 1).arg(p.audioFile);
        if (!p.medianOffsetMs) {
            lines << who + ": no clap both heard and seen";
            continue;
        }
        // Rounded first, so -0.4 reads as 0, not -0.
        const double off = std::round(*p.medianOffsetMs) == 0.0 ? 0.0 : *p.medianOffsetMs;
        lines << QString("%1: audio %2%3 ms from the video over %4 clap(s) (%5)")
                     .arg(who, off >= 0.0 ? "+" : "")
                     .arg(off, 0, 'f', 0)
                     .arg(p.matched)
                     .arg(p.verdict);
    }
    return lines.join("  ·  ");
}

} // namespace mosaic
