
.. _program_listing_file_src_record_record_manager.cpp:

Program Listing for File record_manager.cpp
===========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_record_record_manager.cpp>` (``src\record\record_manager.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "record/record_manager.hpp"
   #include "utils/logger.hpp"
   #include "utils/timestamp.hpp"
   #include <QCoreApplication>
   #include <QDateTime>
   #include <QDir>
   #include <QFile>
   #include <QJsonArray>
   #include <QJsonDocument>
   #include <QJsonObject>
   #include <QTimer>
   
   namespace mosaic {
   
   struct RecordManager::Impl {
       AppSettings&    settings;
       TriggerManager* triggerMgr;
       AudioManager*   audioMgr;
       VideoManager*   videoMgr;
       QString         username;
   
       bool    recording   {false};
       int     elapsedMs   {0};
       int64_t startMs     {0};
       QString sessionPath;
       QTimer  timer;
   
       explicit Impl(AppSettings& s, TriggerManager* tr, AudioManager* au,
                     VideoManager* vi, const QString& user)
           : settings(s), triggerMgr(tr), audioMgr(au), videoMgr(vi), username(user) {}
   };
   
   RecordManager::RecordManager(AppSettings&     settings,
                                 TriggerManager*  triggerMgr,
                                 AudioManager*    audioMgr,
                                 VideoManager*    videoMgr,
                                 const QString&   username,
                                 QObject*         parent)
       : QObject(parent)
       , d(std::make_unique<Impl>(settings, triggerMgr, audioMgr, videoMgr, username))
   {
       d->timer.setInterval(100);
       connect(&d->timer, &QTimer::timeout, this, &RecordManager::tick);
   }
   
   RecordManager::~RecordManager() { stop(); }
   
   // ── Session metadata ────────────────────────────────────────────────────────
   
   void RecordManager::write_session_meta() const {
       const QString path = d->sessionPath + "/session_meta.json";
   
       // Cameras
       QJsonArray cameras;
       for (int i = 0; i < static_cast<int>(d->settings.video.cameras.size()); ++i) {
           const auto& cam = d->settings.video.cameras[static_cast<size_t>(i)];
           const QJsonObject calObj = cam.calibration.to_json();
           cameras.append(QJsonObject{
               {"index",        i},
               {"serial",       cam.serialNumber},
               {"name",         cam.friendlyName},
               {"width",        cam.width},
               {"height",       cam.height},
               {"fps",          cam.fps},
               {"pixel_format", cam.pixelFormat},
               {"codec",        d->settings.video.codec},
               {"calibration",  calObj},
           });
       }
   
       // Microphones
       QJsonArray mics;
       for (int i = 0; i < static_cast<int>(d->settings.audio.microphones.size()); ++i) {
           const auto& mic = d->settings.audio.microphones[static_cast<size_t>(i)];
           mics.append(QJsonObject{
               {"index",       i},
               {"device_id",   mic.deviceId},
               {"name",        mic.friendlyName},
               {"sample_rate", mic.sampleRate},
               {"channels",    mic.channels},
           });
       }
   
       // Trigger sources
       QJsonArray keys, lslInlets, ports;
       for (const auto& k : d->settings.trigger.keyboardTriggers) {
           keys.append(QJsonObject{{"name", k.name}, {"key_seq", k.keySeq}});
       }
       for (const auto& l : d->settings.trigger.lslInlets) {
           lslInlets.append(QJsonObject{{"name", l.name}, {"stream_name", l.streamName}});
       }
       for (const auto& p : d->settings.trigger.parallelPorts) {
           ports.append(QJsonObject{{"port_address", p.portAddress}});
       }
   
       const QJsonObject root{
           {"schema",                "mosaic-session-v1"},
           {"mosaic_version",        QCoreApplication::applicationVersion()},
           {"recorded_by",           d->username},
           {"session_start_utc",     QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
           {"session_start_elapsed_ns", elapsed_ns()},
           {"session_folder",        d->sessionPath},
           {"cameras",               cameras},
           {"microphones",           mics},
           {"room",                  d->settings.room.to_json()},
           {"trigger_sources",       QJsonObject{
               {"keyboard",          keys},
               {"lsl_inlets",        lslInlets},
               {"parallel_ports",    ports},
               {"lsl_outlet_name",   d->settings.trigger.lslOutletName},
           }},
           {"recording",             QJsonObject{
               {"video_enabled",    d->settings.record.enableVideo},
               {"audio_enabled",    d->settings.record.enableAudio},
               {"trigger_enabled",  d->settings.record.enableTrigger},
               {"video_codec",      d->settings.video.codec},
               {"audio_codec",      d->settings.audio.codec},
           }},
       };
   
       QFile file(path);
       if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
           log_error(QString("[RecordManager] Cannot write session metadata: %1").arg(path));
           return;
       }
       file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
       log_info(QString("[RecordManager] Session metadata → %1").arg(path));
   }
   
   // ── Start ──────────────────────────────────────────────────────────────────
   
   bool RecordManager::start() {
       if (d->recording) {
           log_warning("[RecordManager] start() called while already recording.");
           return false;
       }
   
       // 1. Build and create session folder.
       d->sessionPath = build_session_path();
       if (!QDir().mkpath(d->sessionPath)) {
           const QString msg = QString("Cannot create session folder: %1").arg(d->sessionPath);
           log_error(msg);
           emit error_occurred(msg);
           return false;
       }
       log_info(QString("[RecordManager] Session folder: %1").arg(d->sessionPath));
   
       // 2. Write session metadata immediately so it exists even if recording fails.
       write_session_meta();
   
       // 3. Trigger CSV.
       if (d->settings.record.enableTrigger && d->triggerMgr) {
           const QString path = build_file_path(d->settings.record.triggerBasename, "csv");
           d->triggerMgr->start_recording(path);
       }
   
       // 4. Audio recording — written under sessionPath/audio/, not the session
       // root, so a session folder separates media by kind. A failure to create
       // that subfolder must not silently record zero audio while still
       // reporting a successful start — skip audio and surface the error
       // instead, same severity as the session-folder failure above.
       if (d->settings.record.enableAudio && d->audioMgr &&
           !d->settings.audio.microphones.empty()) {
           const QString audioDir = d->sessionPath + "/audio";
           if (!QDir().mkpath(audioDir)) {
               const QString msg = QString("Cannot create audio folder: %1").arg(audioDir);
               log_error(msg);
               emit error_occurred(msg);
           } else {
               d->audioMgr->start(audioDir,
                                   d->settings.record.audioBasename,
                                   d->settings.audio.microphones);
           }
       }
   
       // 5. Video recording — written under sessionPath/video/, same rationale.
       if (d->settings.record.enableVideo && d->videoMgr) {
           const QString videoDir = d->sessionPath + "/video";
           if (!QDir().mkpath(videoDir)) {
               const QString msg = QString("Cannot create video folder: %1").arg(videoDir);
               log_error(msg);
               emit error_occurred(msg);
           } else {
               d->videoMgr->start(videoDir,
                                   d->settings.record.videoBasename,
                                   d->settings.video);
           }
       } else if (d->settings.record.enableVideo) {
           for (int i = 0; i < static_cast<int>(d->settings.video.cameras.size()); ++i) {
               log_info(QString("[RecordManager] Video backend not available for camera %1").arg(i));
           }
       }
   
       // 6. Start elapsed timer.
       d->startMs   = QDateTime::currentMSecsSinceEpoch();
       d->elapsedMs = 0;
       d->recording = true;
       d->timer.start();
   
       log_info("[RecordManager] Recording started.");
       emit recording_started(d->sessionPath);
       return true;
   }
   
   // ── Stop ───────────────────────────────────────────────────────────────────
   
   void RecordManager::stop() {
       if (!d->recording) { return; }
   
       d->timer.stop();
       d->recording = false;
   
       const int     duration = d->elapsedMs;
       const QString path     = d->sessionPath;
   
       // Stop subsystems in reverse order.
       if (d->settings.record.enableVideo && d->videoMgr) {
           d->videoMgr->stop();
       }
   
       if (d->settings.record.enableAudio && d->audioMgr) {
           d->audioMgr->stop();
       }
   
       if (d->settings.record.enableTrigger && d->triggerMgr) {
           d->triggerMgr->stop_recording();
       }
   
       log_info(QString("[RecordManager] Recording stopped. Duration: %1 ms. Files in: %2")
                    .arg(duration).arg(path));
   
       emit recording_stopped(path, duration);
   }
   
   // ── Timer tick ─────────────────────────────────────────────────────────────
   
   void RecordManager::tick() {
       d->elapsedMs = static_cast<int>(
           QDateTime::currentMSecsSinceEpoch() - d->startMs);
       emit elapsed_ms_changed(d->elapsedMs);
   }
   
   // ── Accessors ──────────────────────────────────────────────────────────────
   
   bool    RecordManager::is_recording()        const { return d->recording;   }
   int     RecordManager::elapsed_ms()          const { return d->elapsedMs;   }
   QString RecordManager::current_session_path() const { return d->sessionPath; }
   
   // ── Path helpers ───────────────────────────────────────────────────────────
   
   QString RecordManager::build_session_path() const {
       const auto& rec = d->settings.record;
       QString path = rec.directory;
       if (!path.endsWith('/')) { path += '/'; }
       path += rec.addTimestamp
           ? QDateTime::currentDateTime().toString(rec.timestampFormat)
           : QString("session");
       return QDir::cleanPath(path);
   }
   
   QString RecordManager::build_file_path(const QString& basename,
                                           const QString& ext) const {
       return d->sessionPath + "/" + basename + "." + ext;
   }
   
   } // namespace mosaic
