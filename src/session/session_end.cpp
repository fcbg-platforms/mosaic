#include "session/session_end.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>

namespace mosaic {

namespace {

QString heartbeat_path(const QString& sessionPath) {
    return sessionPath + QLatin1Char('/') + QLatin1String(kHeartbeatFileName);
}

} // namespace

SessionEnd session_end_state(const QJsonObject& sessionMeta, const QDateTime& lastHeartbeatUtc,
                             const QDateTime& nowUtc) {
    const auto it = sessionMeta.constFind(QStringLiteral("session_end"));
    if (it == sessionMeta.constEnd()) {
        return SessionEnd::Unknown;
    }
    if (it->isObject()) {
        return SessionEnd::Clean;
    }
    // A heartbeat slightly in the future (another machine's clock running
    // ahead) still counts as recent — the age is negative, so under the limit.
    if (lastHeartbeatUtc.isValid() && lastHeartbeatUtc.secsTo(nowUtc) < kHeartbeatStaleSec) {
        return SessionEnd::Recording;
    }
    return SessionEnd::Interrupted;
}

QDateTime read_heartbeat(const QString& sessionPath) {
    QFile f(heartbeat_path(sessionPath));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QDateTime t =
        QDateTime::fromString(QString::fromUtf8(f.readAll()).trimmed(), Qt::ISODateWithMs);
    if (t.isValid()) {
        t = t.toUTC();
    }
    return t;
}

bool write_heartbeat(const QString& sessionPath, const QDateTime& nowUtc) {
    QSaveFile f(heartbeat_path(sessionPath));
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(nowUtc.toUTC().toString(Qt::ISODateWithMs).toUtf8());
    return f.commit();
}

void remove_heartbeat(const QString& sessionPath) { QFile::remove(heartbeat_path(sessionPath)); }

double achieved_fps(int64_t frames, int64_t firstElapsedNs, int64_t lastElapsedNs) {
    if (frames < 2 || firstElapsedNs < 0 || lastElapsedNs <= firstElapsedNs) {
        return -1.0;
    }
    return static_cast<double>(frames - 1) * 1e9 /
           static_cast<double>(lastElapsedNs - firstElapsedNs);
}

bool mark_session_ended(const QString& sessionPath, qint64 durationMs, const QDateTime& endUtc,
                        const std::vector<RecordedCamera>& cameras) {
    const QString path = sessionPath + QStringLiteral("/session_meta.json");

    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(in.readAll(), &err);
    in.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    QJsonObject root = doc.object();
    root.insert(QStringLiteral("session_end"),
                QJsonObject{
                    {"utc", endUtc.toUTC().toString(Qt::ISODateWithMs)},
                    {"duration_ms", durationMs},
                    {"ended_cleanly", true},
                });

    if (!cameras.empty()) {
        QJsonArray entries = root.value(QStringLiteral("cameras")).toArray();
        for (int e = 0; e < entries.size(); ++e) {
            QJsonObject entry = entries[e].toObject();
            const int index   = entry.value(QStringLiteral("index")).toInt(-1);
            for (const auto& cam : cameras) {
                if (cam.index != index) {
                    continue;
                }
                entry.insert(QStringLiteral("frames_recorded"), static_cast<qint64>(cam.frames));
                if (cam.frames > 0 && cam.firstElapsedNs >= 0 &&
                    cam.lastElapsedNs >= cam.firstElapsedNs) {
                    const double s =
                        static_cast<double>(cam.lastElapsedNs - cam.firstElapsedNs) / 1e9;
                    entry.insert(QStringLiteral("recorded_seconds"),
                                 std::round(s * 1000.0) / 1000.0);
                }
                const double fps = achieved_fps(cam.frames, cam.firstElapsedNs, cam.lastElapsedNs);
                if (fps > 0.0) {
                    entry.insert(QStringLiteral("achieved_fps"), std::round(fps * 100.0) / 100.0);
                }
                entries[e] = entry;
                break;
            }
        }
        root.insert(QStringLiteral("cameras"), entries);
    }

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return out.commit();
}

} // namespace mosaic
