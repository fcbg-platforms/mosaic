#include "session/preflight.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <algorithm>

#include "video/camera_label.hpp"
#include "video/fps_readout.hpp"

namespace mosaic {

bool PreflightReport::needs_attention() const { return worst() != PreflightLevel::Ok; }

PreflightLevel PreflightReport::worst() const {
    PreflightLevel w = PreflightLevel::Ok;
    for (const auto& item : items) {
        if (item.level == PreflightLevel::Fail) return PreflightLevel::Fail;
        if (item.level == PreflightLevel::Warn) w = PreflightLevel::Warn;
    }
    return w;
}

QStringList PreflightReport::problems() const {
    QStringList out;
    for (const auto& item : items) {
        if (item.level != PreflightLevel::Ok) {
            out << (item.detail.isEmpty() ? item.title : item.title + " — " + item.detail);
        }
    }
    return out;
}

namespace {

QString duration_text(double minutes) {
    if (minutes >= 120.0) {
        return QString("%1 h").arg(minutes / 60.0, 0, 'f', 0);
    }
    if (minutes >= 60.0) {
        return QString("%1 h").arg(minutes / 60.0, 0, 'f', 1);
    }
    return QString("%1 min").arg(std::max(0.0, minutes), 0, 'f', 0);
}

QString gigabytes(qint64 bytes) {
    return QString("%1 GB").arg(static_cast<double>(bytes) / 1e9, 0, 'f', 1);
}

PreflightItem disk_item(const PreflightInput& in) {
    const QString where = in.directory.isEmpty() ? QString() : QString(" (%1)").arg(in.directory);
    if (in.freeBytes < 0) {
        return {PreflightLevel::Warn, "Disk space unknown",
                QString("Could not read the free space for the recordings folder%1.").arg(where)};
    }
    if (!(in.bytesPerSec > 0.0)) {
        return {PreflightLevel::Ok, "Disk",
                QString("%1 free%2").arg(gigabytes(in.freeBytes), where)};
    }
    const double minutes = static_cast<double>(in.freeBytes) / in.bytesPerSec / 60.0;
    const QString rate   = QString("%1%2 MB/s")
                             .arg(in.rateIsRough ? "roughly " : "≈ ")
                             .arg(in.bytesPerSec / 1e6, 0, 'f', 1);
    const QString summary = QString("%1 free%2 — about %3 of recording at %4")
                                .arg(gigabytes(in.freeBytes), where, duration_text(minutes), rate);
    if (minutes < kPreflightDiskFailMinutes) {
        return {PreflightLevel::Fail, "Disk almost full", summary};
    }
    if (minutes < kPreflightDiskWarnMinutes) {
        return {PreflightLevel::Warn, "Disk space low", summary};
    }
    return {PreflightLevel::Ok, "Disk", summary};
}

} // namespace

PreflightReport evaluate_preflight(const PreflightInput& in) {
    QVector<PreflightItem> items;

    // ── Cameras ──────────────────────────────────────────────────────────
    // A row per problem, and one summary row for the rest: six identical
    // "fine" rows would bury the one that is not.
    if (in.videoEnabled) {
        if (in.cameras.isEmpty()) {
            items.push_back({PreflightLevel::Fail, "No cameras configured",
                             "Video recording is on, but there is no camera to record. Add one "
                             "in Settings → Video, or turn video off in Settings → Record."});
        }
        int healthy   = 0;
        int measuring = 0;
        for (const auto& c : in.cameras) {
            const QString name = camera_label(c.configIndex);
            if (!c.opened) {
                items.push_back({PreflightLevel::Fail, name + " is not open",
                                 "It will not be recorded. Check its cable and power, then "
                                 "reconnect it from Settings → Video."});
                continue;
            }
            if (!c.grabberRunning || c.lastFrameAgeSec < 0.0) {
                items.push_back({PreflightLevel::Fail, name + " is not delivering frames",
                                 "It is open but no image has arrived, so its video would be "
                                 "empty."});
                continue;
            }
            if (c.lastFrameAgeSec >= kPreflightStaleFrameSec) {
                items.push_back(
                    {PreflightLevel::Fail, name + " has stopped delivering frames",
                     QString("Its last image arrived %1 s ago — the link may have dropped. Its "
                             "video would stop there.")
                         .arg(c.lastFrameAgeSec, 0, 'f', 0)});
                continue;
            }
            bool fine = true;
            if (c.fixedRate && c.achievableFps > 0.0 && c.configuredFps > 0.0 &&
                c.achievableFps < c.configuredFps * k_fps_shortfall_factor) {
                items.push_back(
                    {PreflightLevel::Warn, name + " is below its frame rate",
                     QString("It can only reach about %1 of the %2 fps set — usually the "
                             "exposure limit or the link.")
                         .arg(c.achievableFps, 0, 'f', 1)
                         .arg(c.configuredFps, 0, 'f', 1)});
                fine = false;
            }
            if (c.wantsAction1 && !c.action1Ready) {
                items.push_back({PreflightLevel::Warn, name + " is not synchronised",
                                 "It is set to hardware trigger (Action1) but is free-running, so "
                                 "its frames will not line up exactly with the other cameras'."});
                fine = false;
            }
            if (fine) {
                ++healthy;
                if (c.fixedRate && !(c.achievableFps > 0.0)) ++measuring;
            }
        }
        if (healthy > 0) {
            QString detail = "Open, delivering frames";
            if (healthy > measuring) detail += ", at their frame rate";
            if (measuring > 0) {
                detail += QString(" (%1 still measuring %2 rate)")
                              .arg(measuring)
                              .arg(measuring == 1 ? "its" : "their");
            }
            items.push_back({PreflightLevel::Ok,
                             QString("%1 camera%2 ready").arg(healthy).arg(healthy == 1 ? "" : "s"),
                             detail});
        }
    }

    // ── Microphones ──────────────────────────────────────────────────────
    if (in.audioEnabled) {
        if (in.configuredMics == 0) {
            items.push_back({PreflightLevel::Warn, "No microphones configured",
                             "Audio recording is on, but there is no microphone to record."});
        } else if (!in.missingMics.isEmpty()) {
            for (const QString& mic : in.missingMics) {
                items.push_back({PreflightLevel::Warn, mic + " not found",
                                 "That device is not connected. The recording would silently use "
                                 "the system's default input instead."});
            }
        } else {
            items.push_back({PreflightLevel::Ok,
                             QString("%1 microphone%2 ready")
                                 .arg(in.configuredMics)
                                 .arg(in.configuredMics == 1 ? "" : "s"),
                             QString()});
        }
    }

    // ── Disk ─────────────────────────────────────────────────────────────
    items.push_back(disk_item(in));

    std::stable_sort(items.begin(), items.end(),
                     [](const PreflightItem& a, const PreflightItem& b) {
                         return static_cast<int>(a.level) > static_cast<int>(b.level);
                     });
    return PreflightReport{items};
}

double estimate_recording_bytes_per_sec(int cameras, int videoKbps, int mics, int sampleRate,
                                        int channels, const QString& audioCodec) {
    const double video = std::max(0, cameras) * std::max(0, videoKbps) * 1000.0 / 8.0;
    double audio       = std::max(0, mics) * std::max(0, sampleRate) * std::max(0, channels) * 2.0;
    if (!audioCodec.startsWith(QLatin1String("pcm"))) {
        audio /= 4.0;
    }
    return video + audio;
}

qint64 free_bytes_for(const QString& directory) {
    QString path =
        QFileInfo(directory.isEmpty() ? QStringLiteral(".") : directory).absoluteFilePath();
    // Walk up to something that exists; QStorageInfo of a missing path is
    // invalid rather than "the volume it would be on".
    while (!QFileInfo::exists(path)) {
        const QString parent = QFileInfo(path).absolutePath();
        if (parent == path) {
            return -1;
        }
        path = parent;
    }
    const QStorageInfo storage(path);
    if (!storage.isValid() || !storage.isReady()) {
        return -1;
    }
    return storage.bytesAvailable();
}

} // namespace mosaic
