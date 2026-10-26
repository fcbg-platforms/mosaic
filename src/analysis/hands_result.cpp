#include "analysis/hands_result.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <algorithm>
#include <limits>

namespace mosaic {

namespace {

double number_or_nan(const QJsonValue& v) {
    return v.isDouble() ? v.toDouble() : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

HandsResult HandsResult::load(const QString& path) {
    HandsResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return r;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QString schema   = root["schema"].toString();
    if (schema != QLatin1String("mosaic-hands-v1") &&
        schema != QLatin1String("mosaic-hands3d-v1")) {
        return r;
    }
    r.is3d_ = schema == QLatin1String("mosaic-hands3d-v1");
    if (r.is3d_) {
        for (const auto& v : root["agreement"].toObject()["camera_pairs"].toArray()) {
            const QJsonObject o  = v.toObject();
            const QJsonArray cam = o["cameras"].toArray();
            r.agreement_ << Agreement{cam.at(0).toInt(-1),
                                      cam.at(1).toInt(-1),
                                      o["hands"].toInt(),
                                      number_or_nan(o["gap_median_mm"]),
                                      number_or_nan(o["offset_mm"]),
                                      number_or_nan(o["scatter_median_mm"]),
                                      number_or_nan(o["frame_gap_median_ms"]),
                                      number_or_nan(o["offset_same_moment_mm"])};
        }
    }
    r.model_  = root["model"].toString();
    r.stride_ = root["stride"].toInt(1);
    // In the room, the kinematics' rate (the ticks, every Nth frame).
    r.fps_            = root[r.is3d_ ? "rate" : "fps"].toDouble();
    r.videoStartMs_   = root["video_start_ms"].toDouble();
    r.annotatedVideo_ = root["annotated_video"].toString();
    r.kinematicsFile_ = root["kinematics_file"].toString();
    r.movementsFile_  = root["movements_file"].toString();
    for (const auto& v : root["people"].toArray()) {
        const QJsonObject o = v.toObject();
        r.people_ << Person{o["subject_id"].toInt(-1), number_or_nan(o["laterality_index"]),
                            number_or_nan(o["both_moving_s"]),
                            number_or_nan(o["both_moving_share"])};
    }
    for (const auto& v : root["hands"].toArray()) {
        const QJsonObject o = v.toObject();
        Hand h;
        // null for a hand no person's wrist was near (MediaPipe).
        h.subjectId    = o["subject_id"].isDouble() ? o["subject_id"].toInt() : -1;
        h.number       = o["hand"].isDouble() ? o["hand"].toInt() : -1;
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
        if (o["kinematics"].isObject()) {
            const QJsonObject k               = o["kinematics"].toObject();
            h.kinematics.valid                = true;
            h.kinematics.measuredS            = number_or_nan(k["measured_s"]);
            h.kinematics.wristPathHl          = number_or_nan(k["wrist_path_hl"]);
            h.kinematics.wristSpeedMedian     = number_or_nan(k["wrist_speed_median_hl_s"]);
            h.kinematics.wristSpeedP95        = number_or_nan(k["wrist_speed_p95_hl_s"]);
            h.kinematics.gripApertureMedian   = number_or_nan(k["grip_aperture_median_palm"]);
            h.kinematics.gripApertureP95      = number_or_nan(k["grip_aperture_p95_palm"]);
            h.kinematics.handOpeningMedian    = number_or_nan(k["hand_opening_median_palm"]);
            h.kinematics.palmMm               = number_or_nan(k["palm_median_mm"]);
            h.kinematics.wristSpeedMedianMm   = number_or_nan(k["wrist_speed_median_mm_s"]);
            h.kinematics.wristSpeedP95Mm      = number_or_nan(k["wrist_speed_p95_mm_s"]);
            h.kinematics.gripApertureMedianMm = number_or_nan(k["grip_aperture_median_mm"]);
        }
        if (o["movements"].isObject()) {
            const QJsonObject m         = o["movements"].toObject();
            h.movements.valid           = true;
            h.movements.count           = m["movements"].toInt();
            h.movements.cutOff          = m["cut_off"].toInt();
            h.movements.durationS       = number_or_nan(m["duration_median_s"]);
            h.movements.peakSpeed       = number_or_nan(m["peak_speed_median_hl_s"]);
            h.movements.timeToPeakPct   = number_or_nan(m["time_to_peak_median_pct"]);
            h.movements.movementUnits   = number_or_nan(m["movement_units_median"]);
            h.movements.sparc           = number_or_nan(m["sparc_median"]);
            h.movements.straightness    = number_or_nan(m["straightness_median"]);
            h.movements.maxGripAperture = number_or_nan(m["max_grip_aperture_median_palm"]);
        }
        r.hands_ << h;
    }
    r.valid_ = true;
    return r;
}

QVector<HandsResult::Movement> HandsResult::load_movements(const QString& path) {
    QVector<Movement> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    QTextStream ts(&f);
    const QStringList header = ts.readLine().split(',');
    const int subject = header.indexOf("subject_id"), side = header.indexOf("side");
    const int handCol = header.indexOf("hand"); // 3D: the hand's number
    const int start = header.indexOf("start_ms"), end = header.indexOf("end_ms");
    const int peak = header.indexOf("peak_speed_hl_s");
    if (subject < 0 || side < 0 || start < 0 || end < 0 || peak < 0) {
        return out;
    }
    while (!ts.atEnd()) {
        const QStringList cells = ts.readLine().split(',');
        if (cells.size() != header.size()) {
            continue;
        }
        bool isId    = false;
        const int id = cells[subject].toInt(&isId);
        out << Movement{handCol < 0 ? -1 : cells[handCol].toInt(),
                        isId ? id : -1,
                        cells[side],
                        cells[start].toDouble(),
                        cells[end].toDouble(),
                        cells[peak].toDouble()};
    }
    return out;
}

QVector<HandsResult::Series> HandsResult::load_kinematics(const QString& path) {
    QVector<Series> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    QTextStream ts(&f);
    const QStringList header = ts.readLine().split(',');
    const auto col           = [&](const char* name) { return header.indexOf(name); };
    const int time = col("time_ms"), subject = col("subject_id"), side = col("side");
    const int hand = col("hand"); // 3D: the hand's number
    if (time < 0 || subject < 0 || side < 0) {
        return out;
    }
    const double nan = std::numeric_limits<double>::quiet_NaN();
    while (!ts.atEnd()) {
        const QStringList cells = ts.readLine().split(',');
        if (cells.size() != header.size()) {
            continue;
        }
        bool isId           = false;
        const int id        = cells[subject].toInt(&isId);
        const int subjectId = isId ? id : -1;
        const int number    = hand < 0 ? -1 : cells[hand].toInt();
        const auto same     = [&](const Series& s) {
            return s.hand == number && s.subjectId == subjectId && s.side == cells[side];
        };
        // Rows come grouped by hand: the last series is nearly always it.
        auto it = !out.isEmpty() && same(out.last()) ? out.end() - 1
                                                     : std::find_if(out.begin(), out.end(), same);
        if (it == out.end()) {
            out.append(Series{number, subjectId, cells[side], {}, {}});
            it = out.end() - 1;
        }
        it->timeMs << cells[time].toDouble();
        for (int c = 0; c < header.size(); ++c) {
            if (c == time || c == subject || c == side || c == hand) {
                continue;
            }
            bool ok        = false;
            const double v = cells[c].toDouble(&ok);
            it->columns[header[c]] << (ok ? v : nan);
        }
    }
    return out;
}

} // namespace mosaic
