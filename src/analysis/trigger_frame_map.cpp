#include "analysis/trigger_frame_map.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <algorithm>
#include <cmath>

#include "utils/logger.hpp"

namespace mosaic {

// ── CSV readers ────────────────────────────────────────────────────────────

QStringList TriggerFrameMap::split_csv_line(const QString& line) {
    QStringList out;
    QString field;
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += c;
            }
        } else {
            if (c == '"') {
                inQuotes = true;
            } else if (c == ',') {
                out.append(field);
                field.clear();
            } else {
                field += c;
            }
        }
    }
    out.append(field);
    return out;
}

QVector<TriggerFrameMap::RawTrigger> TriggerFrameMap::read_trigger_csv(const QString& csvPath,
                                                                       bool& hasElapsedNsColumn) {
    QVector<RawTrigger> out;
    hasElapsedNsColumn = false;

    QFile f(csvPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    QTextStream ts(&f);

    const QStringList header = ts.readLine().split(',');
    const int idxElapsedNs   = header.indexOf("elapsed_ns");
    const int idxElapsedMs   = header.indexOf("elapsed_ms");
    const int idxWallClock   = header.indexOf("wall_clock");
    const int idxSource      = header.indexOf("source");
    const int idxLabel       = header.indexOf("label");
    const int idxValue       = header.indexOf("value");
    const int idxCode        = header.indexOf("code");
    if (idxElapsedNs < 0) {
        return out;
    } // old-format file, predates this column
    hasElapsedNsColumn = true;

    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const QStringList fields = split_csv_line(line);
        if (fields.size() <= idxElapsedNs) {
            continue;
        }

        RawTrigger t;
        t.elapsedNs = fields[idxElapsedNs].toLongLong();
        if (idxElapsedMs >= 0 && idxElapsedMs < fields.size()) {
            t.elapsedMs = fields[idxElapsedMs].toDouble();
        }
        if (idxWallClock >= 0 && idxWallClock < fields.size()) {
            t.wallClock = fields[idxWallClock];
        }
        if (idxSource >= 0 && idxSource < fields.size()) {
            t.source = fields[idxSource];
        }
        if (idxLabel >= 0 && idxLabel < fields.size()) {
            t.label = fields[idxLabel];
        }
        if (idxCode >= 0 && idxCode < fields.size()) {
            t.code = fields[idxCode].toInt();
        }
        if (idxValue >= 0 && idxValue < fields.size()) {
            t.value = fields[idxValue].toDouble();
        }
        out.append(t);
    }
    return out;
}

QVector<TriggerFrameMap::FrameTs> TriggerFrameMap::read_timestamps(const QString& csvPath) {
    QVector<FrameTs> out;
    QFile f(csvPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    QTextStream ts(&f);
    ts.readLine(); // skip header: frame_id,elapsed_ns,wall_ns,hw_timestamp_ns
    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QStringList cols = line.split(',');
        if (cols.size() < 2) {
            continue;
        }
        FrameTs ft;
        ft.frameId   = cols[0].toInt();
        ft.elapsedNs = cols[1].toLongLong();
        out.append(ft);
    }
    return out;
}

// ── generate ─────────────────────────────────────────────────────────────

TriggerFrameMap TriggerFrameMap::generate(const QString& sessionPath) {
    TriggerFrameMap m;

    bool hasElapsedNsColumn = false;
    auto rawTriggers =
        read_trigger_csv(QDir(sessionPath).filePath("trigger.csv"), hasElapsedNsColumn);
    if (!hasElapsedNsColumn || rawTriggers.isEmpty()) {
        return m;
    }

    // Defensive: guarantee ascending elapsed_ns order for the two-pointer
    // merge below, regardless of any edge case in dispatch/write ordering
    // (stable_sort so ties keep their original file order).
    std::stable_sort(
        rawTriggers.begin(), rawTriggers.end(),
        [](const RawTrigger& a, const RawTrigger& b) { return a.elapsedNs < b.elapsedNs; });

    const QDir videoDir(sessionPath + "/video");
    constexpr int kMaxCams = 16;
    QVector<QVector<FrameTs>> camTs;
    for (int i = 0; i < kMaxCams; ++i) {
        auto ts = read_timestamps(videoDir.filePath(QString("timestamps_cam%1.csv").arg(i)));
        if (ts.isEmpty()) {
            break;
        }

        TriggerFrameCamera tc;
        tc.index          = i;
        tc.videoFile      = QString("video/video_%1.mp4").arg(i);
        tc.framesCaptured = static_cast<int>(ts.size());
        m.cameras_.append(tc);
        camTs.append(std::move(ts));
    }
    if (m.cameras_.isEmpty()) {
        return m;
    }

    m.generatedAt_ = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    m.rows_.reserve(rawTriggers.size());

    // Two-pointer nearest-neighbour per camera — both rawTriggers and each
    // camera's frame list are ascending on elapsed_ns, same technique
    // SyncManifest::generate() already uses for its own tick loop.
    QVector<int> ptr(m.cameras_.size(), 0);

    for (int r = 0; r < rawTriggers.size(); ++r) {
        const auto& rt = rawTriggers[r];
        TriggerFrameRow row;
        row.rowIndex  = r;
        row.elapsedNs = rt.elapsedNs;
        row.elapsedMs = rt.elapsedMs;
        row.wallClock = rt.wallClock;
        row.source    = rt.source;
        row.label     = rt.label;
        row.code      = rt.code;
        row.value     = rt.value;

        for (int c = 0; c < m.cameras_.size(); ++c) {
            const auto& frames = camTs[c];
            TriggerFrameHit hit;
            hit.cameraIndex = c;
            if (frames.isEmpty()) {
                row.frames.append(hit); // frameId stays -1: no data for this camera
                continue;
            }

            // Strict '<' (not '<='), matching SyncManifest::generate()'s own
            // tie-break exactly: an exact tie stays on the earlier frame
            // rather than advancing to the later one.
            int& p = ptr[c];
            while (p + 1 < frames.size() && std::llabs(frames[p + 1].elapsedNs - rt.elapsedNs) <
                                                std::llabs(frames[p].elapsedNs - rt.elapsedNs)) {
                ++p;
            }
            hit.frameId         = frames[p].frameId;
            hit.deltaMs         = static_cast<double>(frames[p].elapsedNs - rt.elapsedNs) / 1e6;
            hit.videoPositionMs = (frames[p].elapsedNs - frames.first().elapsedNs) / 1'000'000;
            row.frames.append(hit);
        }
        m.rows_.append(std::move(row));
    }

    m.valid_ = true;
    return m;
}

// ── save ─────────────────────────────────────────────────────────────────

bool TriggerFrameMap::save(const QString& sessionPath) const {
    QJsonObject root;
    root["schema"]           = "mosaic-trigger-frame-map-v1";
    root["session_path"]     = sessionPath;
    root["generated_at_utc"] = generatedAt_;

    QJsonArray cams;
    for (const auto& c : cameras_) {
        cams.append(QJsonObject{
            {"index", c.index},
            {"video_file", c.videoFile},
            {"frames_captured", c.framesCaptured},
        });
    }
    root["cameras"] = cams;

    QJsonArray triggers;
    for (const auto& row : rows_) {
        QJsonArray frames;
        for (const auto& hit : row.frames) {
            frames.append(QJsonObject{
                {"camera_index", hit.cameraIndex},
                {"frame_id", hit.frameId},
                {"delta_ms", hit.deltaMs},
                {"video_position_ms", hit.videoPositionMs},
            });
        }
        triggers.append(QJsonObject{
            {"row", row.rowIndex},
            {"elapsed_ns", QString::number(row.elapsedNs)},
            {"elapsed_ms", row.elapsedMs},
            {"wall_clock", row.wallClock},
            {"source", row.source},
            {"label", row.label},
            {"code", row.code},
            {"value", row.value},
            {"frames", frames},
        });
    }
    root["triggers"] = triggers;

    const QString outPath = QDir(sessionPath).filePath("trigger_frame_map.json");
    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly)) {
        log_warning("[TriggerFrameMap] Cannot write " + outPath);
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    log_info("[TriggerFrameMap] Saved → " + outPath);
    return true;
}

// ── load ─────────────────────────────────────────────────────────────────

TriggerFrameMap TriggerFrameMap::load(const QString& sessionPath) {
    TriggerFrameMap m;
    QFile f(QDir(sessionPath).filePath("trigger_frame_map.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        return m;
    }

    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.isEmpty()) {
        return m;
    }

    m.generatedAt_ = root["generated_at_utc"].toString();

    for (const auto& cv : root["cameras"].toArray()) {
        const QJsonObject o = cv.toObject();
        TriggerFrameCamera c;
        c.index          = o["index"].toInt();
        c.videoFile      = o["video_file"].toString();
        c.framesCaptured = o["frames_captured"].toInt();
        m.cameras_.append(c);
    }

    for (const auto& tv : root["triggers"].toArray()) {
        const QJsonObject o = tv.toObject();
        TriggerFrameRow row;
        row.rowIndex  = o["row"].toInt();
        row.elapsedNs = o["elapsed_ns"].toString().toLongLong();
        row.elapsedMs = o["elapsed_ms"].toDouble();
        row.wallClock = o["wall_clock"].toString();
        row.source    = o["source"].toString();
        row.label     = o["label"].toString();
        row.code      = o["code"].toInt();
        row.value     = o["value"].toDouble();

        for (const auto& fv : o["frames"].toArray()) {
            const QJsonObject fo = fv.toObject();
            TriggerFrameHit hit;
            hit.cameraIndex     = fo["camera_index"].toInt(-1);
            hit.frameId         = fo["frame_id"].toInt(-1);
            hit.deltaMs         = fo["delta_ms"].toDouble();
            hit.videoPositionMs = static_cast<int64_t>(fo["video_position_ms"].toInteger(-1));
            row.frames.append(hit);
        }
        m.rows_.append(std::move(row));
    }

    m.valid_ = !m.cameras_.isEmpty() && !m.rows_.isEmpty();
    return m;
}

// ── export_csv ───────────────────────────────────────────────────────────

bool TriggerFrameMap::export_csv(const QString& csvPath) const {
    QFile f(csvPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream out(&f);

    out << "trigger_row,elapsed_ns,elapsed_ms,wall_clock,source,label,value,code";
    for (const auto& c : cameras_) {
        out << QString(",cam%1_frame_id,cam%1_delta_ms").arg(c.index);
    }
    out << "\n";

    for (const auto& row : rows_) {
        out << row.rowIndex << "," << row.elapsedNs << "," << QString::number(row.elapsedMs, 'f', 3)
            << "," << row.wallClock << "," << row.source << ","
            << "\"" << QString(row.label).replace('"', "\"\"") << "\","
            << QString::number(row.value, 'f', 6) << "," << row.code;
        for (const auto& hit : row.frames) {
            out << "," << hit.frameId << "," << QString::number(hit.deltaMs, 'f', 3);
        }
        out << "\n";
    }
    return true;
}

// ── queries ───────────────────────────────────────────────────────────────

bool TriggerFrameMap::is_valid() const { return valid_; }
int TriggerFrameMap::camera_count() const { return static_cast<int>(cameras_.size()); }
int TriggerFrameMap::trigger_count() const { return static_cast<int>(rows_.size()); }

const TriggerFrameCamera& TriggerFrameMap::camera_info(int idx) const { return cameras_[idx]; }
const TriggerFrameRow& TriggerFrameMap::row(int idx) const { return rows_[idx]; }
const QVector<TriggerFrameRow>& TriggerFrameMap::rows() const { return rows_; }

} // namespace mosaic
