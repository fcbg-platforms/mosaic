#include "analysis/social_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <limits>

namespace mosaic {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::optional<double> opt(const QJsonValue& v) {
    return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
}

QVector<double> numbers(const QJsonArray& a) {
    QVector<double> out;
    out.reserve(a.size());
    for (const auto& v : a) {
        out << (v.isDouble() ? v.toDouble() : kNaN);
    }
    return out;
}

} // namespace

SocialResult SocialResult::load(const QString& path) {
    SocialResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return r;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root["schema"].toString() != QLatin1String("mosaic-social-v1")) {
        return r;
    }
    const QJsonObject cams = root["cameras"].toObject();
    r.cameraA_             = cams["a"].toInt(-1);
    r.cameraB_             = cams["b"].toInt(-1);
    const QJsonObject vs   = root["video_start_ms"].toObject();
    r.startA_              = vs["a"].isDouble() ? vs["a"].toDouble() : kNaN;
    r.startB_              = vs["b"].isDouble() ? vs["b"].toDouble() : kNaN;

    const QJsonObject sync = root["synchrony"].toObject();
    // A fixed order, so the chart's choices do not depend on the file's.
    for (const char* id : {"head", "body", "heart_rate"}) {
        if (!sync.contains(id)) {
            continue;
        }
        const QJsonObject m = sync[id].toObject();
        const QJsonObject s = m["summary"].toObject();
        const QJsonObject e = m["series"].toObject();
        Measure x;
        x.id                 = id;
        x.t0Ms               = m["t0_ms"].toDouble();
        x.tS                 = numbers(e["t_s"].toArray());
        x.peakR              = numbers(e["peak_r"].toArray());
        x.lagS               = numbers(e["peak_lag_s"].toArray());
        x.meanPeakR          = s["mean_peak_r"].toDouble(kNaN);
        x.surrogateMeanPeakR = opt(s["surrogate_mean_peak_r"]);
        x.surrogateP95       = opt(s["surrogate_p95"]);
        x.synchrony          = opt(s["synchrony"]);
        x.pValue             = opt(s["p_value"]);
        x.aLeadsPct          = opt(s["a_leads_pct"]);
        x.bLeadsPct          = opt(s["b_leads_pct"]);
        x.medianLagS         = opt(s["median_lag_s"]);
        x.windows            = s["windows"].toInt();
        r.measures_ << x;
    }

    const QJsonObject mim = root["mimicry"].toObject();
    for (const char* kind : {"smile", "nod"}) {
        if (!mim.contains(kind)) {
            continue;
        }
        const QJsonObject k = mim[kind].toObject();
        for (const bool aToB : {true, false}) {
            const QJsonObject o = k[aToB ? "a_to_b" : "b_to_a"].toObject();
            Mimicry m;
            m.kind       = kind;
            m.aToB       = aToB;
            m.events     = o["events"].toInt();
            m.followed   = o["followed"].toInt();
            m.rate       = opt(o["rate"]);
            m.chanceRate = opt(o["chance_rate"]);
            m.pValue     = opt(o["p_value"]);
            r.mimicry_ << m;
        }
    }
    for (const auto& n : root["notes"].toArray()) {
        r.notes_ << n.toString();
    }
    r.valid_ = true;
    return r;
}

double SocialResult::video_start_ms(int camera) const {
    if (camera == cameraA_) {
        return startA_;
    }
    if (camera == cameraB_) {
        return startB_;
    }
    return kNaN;
}

const SocialResult::Measure* SocialResult::measure(const QString& id) const {
    for (const auto& m : measures_) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

} // namespace mosaic
