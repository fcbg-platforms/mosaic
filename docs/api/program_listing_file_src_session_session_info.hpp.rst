
.. _program_listing_file_src_session_session_info.hpp:

Program Listing for File session_info.hpp
=========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_session_session_info.hpp>` (``src\session\session_info.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

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
   
   namespace mosaic {
   
   // ── AnnotationCategory ─────────────────────────────────────────────────────
   
   struct AnnotationCategory {
       QString name;
       QColor  color;
   
       static QList<AnnotationCategory> defaults() {
           return {
               {"Grooming",     QColor("#e8865a")},
               {"Exploration",  QColor("#5ab4e8")},
               {"Social",       QColor("#a05ae8")},
               {"Rearing",      QColor("#e8c05a")},
               {"Freezing",     QColor("#5ae8c0")},
               {"Vocalization", QColor("#e85a8a")},
               {"Eating",       QColor("#88e85a")},
               {"Drinking",     QColor("#5a8ae8")},
               {"Custom",       QColor("#909090")},
           };
       }
   
       static QColor color_for(const QString& name) {
           for (const auto& c : defaults()) {
               if (c.name == name) { return c.color; }
           }
           return QColor("#909090");
       }
   };
   
   // ── Annotation ─────────────────────────────────────────────────────────────
   
   struct Annotation {
       int64_t timestampMs = 0;   // ms from session start (0 = session begin)
       QString category;
       QString note;
   
       QJsonObject to_json() const {
           return {
               {"timestamp_ms", timestampMs},
               {"category",     category},
               {"note",         note},
           };
       }
   
       static Annotation from_json(const QJsonObject& o) {
           Annotation a;
           a.timestampMs = static_cast<int64_t>(
               o["timestamp_ms"].toVariant().toLongLong());
           a.category = o["category"].toString();
           a.note     = o["note"].toString();
           return a;
       }
   };
   
   // ── SessionInfo ────────────────────────────────────────────────────────────
   
   struct SessionInfo {
       QString   path;
       QString   name;             // directory basename
       QString   recordedBy;
       QDateTime startUtc;
       int       durationMs    = -1;
       int       cameraCount   = 0;
       int       micCount      = 0;
       QString   mosaicVersion;
       QString   videoCodec;
       QString   audioCodec;
       bool      hasPoseAnalysis   = false;
       bool      hasMotionAnalysis = false;
       bool      hasTranscript     = false;
       bool      hasExpression     = false;
       bool      hasGazeFusion     = false;
       QStringList videoFiles;
       QStringList audioFiles;
       QStringList analysisFiles;
       QList<Annotation> annotations;
   
       // Load from a session directory.
       static SessionInfo load(const QString& dir) {
           SessionInfo info;
           info.path = QDir::cleanPath(dir);
           info.name = QDir(dir).dirName();
   
           QFile metaFile(dir + "/session_meta.json");
           if (metaFile.open(QIODevice::ReadOnly)) {
               const auto doc  = QJsonDocument::fromJson(metaFile.readAll());
               const auto root = doc.object();
               info.recordedBy    = root["recorded_by"].toString();
               info.mosaicVersion = root["mosaic_version"].toString();
               info.startUtc = QDateTime::fromString(
                   root["session_start_utc"].toString(), Qt::ISODateWithMs);
               info.startUtc.setTimeZone(QTimeZone::utc());
               info.cameraCount = root["cameras"].toArray().size();
               info.micCount    = root["microphones"].toArray().size();
               const auto rec   = root["recording"].toObject();
               info.videoCodec  = rec["video_codec"].toString();
               info.audioCodec  = rec["audio_codec"].toString();
           }
   
           // Video/audio recordings live under video/ and audio/ subfolders;
           // per-video pose/motion output (written beside its source video by
           // the analysis scripts) lands in video/ too, while session-level
           // aggregate motion output (motion_results.*, motion_heatmap.png,
           // …) stays at the session root — so analysis files are scanned in
           // both places. Entries are stored as paths relative to the session
           // dir (e.g. "video/video_0.mp4") so callers can join them onto
           // `path` directly without needing to know which subfolder a given
           // file lives in.
           const auto classify = [&info](const QDir& scanDir, const QString& prefix) {
               if (!scanDir.exists()) { return; }
               for (const auto& fi : scanDir.entryInfoList(QDir::Files, QDir::Name)) {
                   const QString sfx = fi.suffix().toLower();
                   const QString fn  = prefix + fi.fileName();
                   if (sfx == "mp4" || sfx == "mkv" || sfx == "avi") {
                       info.videoFiles << fn;
                   } else if (sfx == "wav" || sfx == "flac") {
                       info.audioFiles << fn;
                   } else if (fi.fileName().contains("pose") || fi.fileName().contains("motion") ||
                              fi.fileName().contains("keypoint") || fi.fileName().contains("heatmap") ||
                              fi.fileName().contains("trajectory") || fi.fileName().contains("velocity") ||
                              fi.fileName().contains("transcript") || fi.fileName().contains("expression") ||
                              fi.fileName().contains("gaze")) {
                       info.analysisFiles << fn;
                       if (fi.fileName().contains("pose"))       { info.hasPoseAnalysis   = true; }
                       if (fi.fileName().contains("motion"))     { info.hasMotionAnalysis = true; }
                       if (fi.fileName().contains("transcript")) { info.hasTranscript     = true; }
                       if (fi.fileName().contains("expression")) { info.hasExpression     = true; }
                       if (fi.fileName().contains("gaze"))       { info.hasGazeFusion     = true; }
                   }
               }
           };
           classify(QDir(dir), "");
           classify(QDir(dir + "/video"), "video/");
           classify(QDir(dir + "/audio"), "audio/");
   
           // Approximate duration: video file mtime vs session start
           if (info.startUtc.isValid()) {
               qint64 maxMtime = 0;
               for (const auto& fn : info.videoFiles) {
                   const QFileInfo fi2(dir + "/" + fn);
                   const auto mt = fi2.lastModified().toSecsSinceEpoch();
                   if (mt > maxMtime) { maxMtime = mt; }
               }
               if (maxMtime > 0) {
                   const qint64 startSec = info.startUtc.toSecsSinceEpoch();
                   if (maxMtime > startSec) {
                       info.durationMs = static_cast<int>((maxMtime - startSec) * 1000);
                   }
               }
           }
   
           info.load_annotations();
           return info;
       }
   
       // List all valid sessions under rootDir, newest first.
       static QList<SessionInfo> list_all(const QString& rootDir) {
           QList<SessionInfo> result;
           const QDir root(rootDir);
           if (!root.exists()) { return result; }
           const auto dirs = root.entryInfoList(
               QDir::Dirs | QDir::NoDotAndDotDot,
               QDir::Name | QDir::Reversed);
           for (const auto& di : dirs) {
               if (QFile::exists(di.filePath() + "/session_meta.json")) {
                   result.append(load(di.filePath()));
               }
           }
           return result;
       }
   
       void load_annotations() {
           annotations.clear();
           QFile f(path + "/annotations.json");
           if (!f.open(QIODevice::ReadOnly)) { return; }
           const auto arr = QJsonDocument::fromJson(f.readAll()).array();
           for (const auto& v : arr) {
               annotations.append(Annotation::from_json(v.toObject()));
           }
       }
   
       void save_annotations() const {
           QJsonArray arr;
           for (const auto& a : annotations) { arr.append(a.to_json()); }
           QFile f(path + "/annotations.json");
           if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
               f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
           }
       }
   
       QString format_duration() const {
           if (durationMs < 0) { return "—"; }
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
