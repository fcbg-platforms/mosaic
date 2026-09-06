#pragma once
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <algorithm>

#include "session/session_name.hpp"

namespace mosaic {

// ── AnnotationCategory ─────────────────────────────────────────────────────

struct AnnotationCategory {
    QString name;
    QColor color;

    static QList<AnnotationCategory> defaults() {
        return {
            {"Grooming", QColor("#e8865a")}, {"Exploration", QColor("#5ab4e8")},
            {"Social", QColor("#a05ae8")},   {"Rearing", QColor("#e8c05a")},
            {"Freezing", QColor("#5ae8c0")}, {"Vocalization", QColor("#e85a8a")},
            {"Eating", QColor("#88e85a")},   {"Drinking", QColor("#5a8ae8")},
            {"Custom", QColor("#909090")},
        };
    }

    static QColor color_for(const QString& name) {
        for (const auto& c : defaults()) {
            if (c.name == name) {
                return c.color;
            }
        }
        return QColor("#909090");
    }
};

// ── Annotation ─────────────────────────────────────────────────────────────

struct Annotation {
    int64_t timestampMs = 0; // ms from session start (0 = session begin)
    QString category;
    QString note;

    QJsonObject to_json() const {
        return {
            {"timestamp_ms", timestampMs},
            {"category", category},
            {"note", note},
        };
    }

    static Annotation from_json(const QJsonObject& o) {
        Annotation a;
        a.timestampMs = static_cast<int64_t>(o["timestamp_ms"].toVariant().toLongLong());
        a.category    = o["category"].toString();
        a.note        = o["note"].toString();
        return a;
    }
};

// ── SessionInfo ────────────────────────────────────────────────────────────

struct SessionInfo {
    QString path;
    QString name; // directory basename
    QString recordedBy;
    QDateTime startUtc;
    int durationMs  = -1;
    int cameraCount = 0;
    int micCount    = 0;
    QString mosaicVersion;
    QString videoCodec;
    QString audioCodec;
    bool hasPoseAnalysis   = false;
    bool hasMotionAnalysis = false;
    bool hasTranscript     = false;
    bool hasExpression     = false;
    bool hasGazeFusion     = false;
    bool hasSkeleton3D     = false;
    bool hasRppg           = false;
    bool hasGaze2d         = false;
    bool hasSyncRepair     = false;
    QStringList videoFiles;
    QStringList audioFiles;
    QStringList analysisFiles;
    QList<Annotation> annotations;

    /// Free-text operator note for the whole session, from notes.txt.
    /// Session-level prose, as opposed to an Annotation, which is pinned to a
    /// moment inside the recording.
    QString notes;

    // Load from a session directory.
    static SessionInfo load(const QString& dir) {
        SessionInfo info;
        info.path = QDir::cleanPath(dir);
        info.name = QDir(dir).dirName();

        QFile metaFile(dir + "/session_meta.json");
        if (metaFile.open(QIODevice::ReadOnly)) {
            const auto doc     = QJsonDocument::fromJson(metaFile.readAll());
            const auto root    = doc.object();
            info.recordedBy    = root["recorded_by"].toString();
            info.mosaicVersion = root["mosaic_version"].toString();
            info.startUtc =
                QDateTime::fromString(root["session_start_utc"].toString(), Qt::ISODateWithMs);
            info.startUtc.setTimeZone(QTimeZone::utc());
            info.cameraCount = root["cameras"].toArray().size();
            info.micCount    = root["microphones"].toArray().size();
            const auto rec   = root["recording"].toObject();
            info.videoCodec  = rec["video_codec"].toString();
            info.audioCodec  = rec["audio_codec"].toString();
        }

        // Video/audio recordings live under video/ and audio/ subfolders.
        // Pose output has its own pose/ subfolder (analysis/run_pose.py's
        // pose_dir), Expression has its own expression/ subfolder
        // (analysis/run_expression.py's expression_dir), rPPG has its own
        // rppg/ subfolder (analysis/run_rppg.py's rppg_dir), and 2D Gaze has
        // its own gaze2d/ subfolder (analysis/run_gaze2d.py's gaze2d_dir);
        // per-video motion output still lands beside its source video in
        // video/; session-level aggregate motion output (motion_results.*,
        // motion_heatmap.png, …) stays at the session root — so analysis
        // files are scanned across all seven locations. Entries are stored
        // as paths relative to the session dir (e.g. "video/video_0.mp4",
        // "pose/video_0.pose.json") so callers can join them onto `path`
        // directly without needing to know which subfolder a given file
        // lives in.
        //
        // anonymized/ (Face Masking output) and synced/ (Frame Sync Repair
        // output) are deliberately NOT among these locations — see the
        // comment at this function's own classify() call sites below for
        // why (they hold full copy videos, not analysis sidecars, and
        // scanning them here would corrupt videoFiles's 1:1 camera-index
        // mapping).
        const auto classify = [&info](const QDir& scanDir, const QString& prefix) {
            if (!scanDir.exists()) {
                return;
            }
            for (const auto& fi : scanDir.entryInfoList(QDir::Files, QDir::Name)) {
                const QString sfx = fi.suffix().toLower();
                const QString fn  = prefix + fi.fileName();
                if (sfx == "mp4" || sfx == "mkv" || sfx == "avi") {
                    info.videoFiles << fn;
                } else if (sfx == "wav" || sfx == "flac") {
                    info.audioFiles << fn;
                } else if (fi.fileName().contains("pose") || fi.fileName().contains("motion") ||
                           fi.fileName().contains("keypoint") ||
                           fi.fileName().contains("heatmap") ||
                           fi.fileName().contains("trajectory") ||
                           fi.fileName().contains("velocity") ||
                           fi.fileName().contains("transcript") ||
                           fi.fileName().contains("expression") || fi.fileName().contains("gaze") ||
                           fi.fileName().contains("skeleton") || fi.fileName().contains("rppg")) {
                    info.analysisFiles << fn;
                    if (fi.fileName().contains("pose")) {
                        info.hasPoseAnalysis = true;
                    }
                    if (fi.fileName().contains("motion")) {
                        info.hasMotionAnalysis = true;
                    }
                    if (fi.fileName().contains("transcript")) {
                        info.hasTranscript = true;
                    }
                    if (fi.fileName().contains("expression")) {
                        info.hasExpression = true;
                    }
                    // Checked before the plain "gaze" substring below —
                    // "video_0.gaze2d.json" also contains "gaze", and would
                    // otherwise be mis-classified as Multi-Camera Gaze
                    // Fusion output too.
                    if (fi.fileName().contains("gaze2d")) {
                        info.hasGaze2d = true;
                    } else if (fi.fileName().contains("gaze")) {
                        info.hasGazeFusion = true;
                    }
                    if (fi.fileName().contains("skeleton")) {
                        info.hasSkeleton3D = true;
                    }
                    if (fi.fileName().contains("rppg")) {
                        info.hasRppg = true;
                    }
                }
            }
        };
        classify(QDir(dir), "");
        classify(QDir(dir + "/video"), "video/");
        classify(QDir(dir + "/audio"), "audio/");
        classify(QDir(dir + "/pose"), "pose/");
        classify(QDir(dir + "/expression"), "expression/");
        classify(QDir(dir + "/rppg"), "rppg/");
        classify(QDir(dir + "/gaze2d"), "gaze2d/");
        // synced/ (Frame Sync Repair output) and anonymized/ (Face Masking
        // output) are deliberately NEVER scanned via classify() — both
        // contain full COPY videos (video_N.mp4), and classify()'s
        // mp4/mkv/avi branch above adds any match to info.videoFiles
        // regardless of `prefix`, which would corrupt cameraCombo's 1:1
        // camera-index mapping used by every Analysis-tab plugin (see
        // AnalysisTabW::rebuild_session_list()). hasSyncRepair is set via a
        // narrow, standalone existence check below instead.
        //
        // The summary JSON is written even when every camera failed or was
        // skipped, so its presence alone would light the SYNCED badge for a
        // session that has no repaired video at all. run_sync_repair.py
        // deletes a failed camera's output, so "at least one synced/
        // video_*.mp4 survives" is the cheap, accurate test for "something
        // was actually repaired" — no JSON parsing needed on this hot
        // session-listing path.
        info.hasSyncRepair =
            QFileInfo::exists(dir + "/synced/sync_repair.json") &&
            !QDir(dir + "/synced").entryList({"video_*.mp4"}, QDir::Files).isEmpty();

        // Approximate duration: video file mtime vs session start
        if (info.startUtc.isValid()) {
            qint64 maxMtime = 0;
            for (const auto& fn : info.videoFiles) {
                const QFileInfo fi2(dir + "/" + fn);
                const auto mt = fi2.lastModified().toSecsSinceEpoch();
                if (mt > maxMtime) {
                    maxMtime = mt;
                }
            }
            if (maxMtime > 0) {
                const qint64 startSec = info.startUtc.toSecsSinceEpoch();
                if (maxMtime > startSec) {
                    info.durationMs = static_cast<int>((maxMtime - startSec) * 1000);
                }
            }
        }

        info.load_annotations();
        info.load_notes();
        return info;
    }

    // Order a session list newest first, by recorded start time.
    //
    // This used to be done by listing directories in reversed *name* order,
    // which was only ever accidentally chronological: "yyyy-MM-dd_hh-mm-ss"
    // happens to sort lexicographically the same way it sorts in time. A
    // BIDS-style "sub-..." prefix destroys that coincidence and would silently
    // reorder the browsers by subject instead, so ordering now uses startUtc —
    // which is what "newest first" always meant. The predicate itself lives in
    // session_name.hpp because this header pulls in QtGui (QColor, for
    // Annotation) and so can never be unit-tested.
    static void sort_newest_first(QList<SessionInfo>& sessions) {
        std::stable_sort(sessions.begin(), sessions.end(),
                         [](const SessionInfo& a, const SessionInfo& b) {
                             return session_entry_newer(a.startUtc, a.name, b.startUtc, b.name);
                         });
    }

    // List all valid sessions under rootDir, newest first.
    static QList<SessionInfo> list_all(const QString& rootDir) {
        QList<SessionInfo> result;
        const QDir root(rootDir);
        if (!root.exists()) {
            return result;
        }
        // QDir::Name only for a deterministic scan order; the real ordering is
        // applied below, once each session's start time has been read.
        const auto dirs = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const auto& di : dirs) {
            if (QFile::exists(di.filePath() + "/session_meta.json")) {
                result.append(load(di.filePath()));
            }
        }
        sort_newest_first(result);
        return result;
    }

    // Kept out of session_meta.json deliberately: that file is written once,
    // before any data exists, and holds calibration matrices and time origins.
    // Notes have to stay editable afterwards — the useful one is usually
    // written after the session — and rewriting the provenance file to fix a
    // sentence risks taking the rest with it. Same split as annotations.json.
    void load_notes() {
        notes.clear();
        QFile f(path + "/notes.txt");
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            notes = QString::fromUtf8(f.readAll());
        }
    }

    void save_notes() const {
        QFile f(path + "/notes.txt");
        const QString trimmed = notes.trimmed();
        if (trimmed.isEmpty()) {
            f.remove(); // emptied on purpose — don't leave a stale note behind
            return;
        }
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write(trimmed.toUtf8());
        }
    }

    void load_annotations() {
        annotations.clear();
        QFile f(path + "/annotations.json");
        if (!f.open(QIODevice::ReadOnly)) {
            return;
        }
        const auto arr = QJsonDocument::fromJson(f.readAll()).array();
        for (const auto& v : arr) {
            annotations.append(Annotation::from_json(v.toObject()));
        }
    }

    void save_annotations() const {
        QJsonArray arr;
        for (const auto& a : annotations) {
            arr.append(a.to_json());
        }
        QFile f(path + "/annotations.json");
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
        }
    }

    QString format_duration() const {
        if (durationMs < 0) {
            return "—";
        }
        const int secs  = durationMs / 1000;
        const int mins  = secs / 60;
        const int hours = mins / 60;
        if (hours > 0) {
            return QString("%1h %2m").arg(hours).arg(mins % 60);
        }
        return QString("%1m %2s").arg(mins).arg(secs % 60);
    }

    // Format milliseconds into HH:MM:SS string.
    static QString ms_to_hms(int64_t ms) {
        const int64_t secs = ms / 1000;
        const int64_t h    = secs / 3600;
        const int64_t m    = (secs % 3600) / 60;
        const int64_t s    = secs % 60;
        return QString("%1:%2:%3")
            .arg(h, 2, 10, QChar('0'))
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0'));
    }
};

} // namespace mosaic
