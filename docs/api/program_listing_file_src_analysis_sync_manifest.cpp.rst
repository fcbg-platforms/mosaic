
.. _program_listing_file_src_analysis_sync_manifest.cpp:

Program Listing for File sync_manifest.cpp
==========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_analysis_sync_manifest.cpp>` (``src\analysis\sync_manifest.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "analysis/sync_manifest.hpp"
   #include "utils/logger.hpp"
   #include <QDateTime>
   #include <QDir>
   #include <QFile>
   #include <QJsonArray>
   #include <QJsonDocument>
   #include <QJsonObject>
   #include <QTextStream>
   #include <QTimeZone>
   #include <algorithm>
   #include <cmath>
   #include <limits>
   
   namespace mosaic {
   
   // ── CSV reader ────────────────────────────────────────────────────────────
   
   QVector<SyncManifest::FrameTs> SyncManifest::read_timestamps(const QString& csvPath)
   {
       QVector<FrameTs> out;
       QFile f(csvPath);
       if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { return out; }
       QTextStream ts(&f);
       ts.readLine();                      // skip header: frame_id,elapsed_ns,wall_ns
       while (!ts.atEnd()) {
           const QString line = ts.readLine().trimmed();
           if (line.isEmpty()) { continue; }
           const QStringList cols = line.split(',');
           if (cols.size() < 3) { continue; }
           FrameTs ft;
           ft.frameId   = cols[0].toInt();
           ft.elapsedNs = cols[1].toLongLong();
           ft.wallNs    = cols[2].toLongLong();
           out.append(ft);
       }
       return out;
   }
   
   // ── Session-start wall clock from session_meta.json ───────────────────────
   
   int64_t SyncManifest::session_start_wall_ns(const QString& sessionPath)
   {
       QFile f(QDir(sessionPath).filePath("session_meta.json"));
       if (!f.open(QIODevice::ReadOnly)) { return 0; }
       const auto obj = QJsonDocument::fromJson(f.readAll()).object();
       const QString iso = obj["session_start_utc"].toString();
       if (iso.isEmpty()) { return 0; }
       QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
       dt.setTimeZone(QTimeZone::utc());
       return dt.toMSecsSinceEpoch() * 1'000'000LL;
   }
   
   // ── generate ─────────────────────────────────────────────────────────────
   
   SyncManifest SyncManifest::generate(const QString& sessionPath, double masterFps)
   {
       SyncManifest m;
       m.masterFps_ = masterFps;
       m.stepNs_    = static_cast<int64_t>(1e9 / masterFps + 0.5);
   
       const QDir videoDir(sessionPath + "/video");
   
       // Load per-camera timestamp files (cam0, cam1, … until first miss).
       const int kMaxCams = 16;
       QVector<QVector<FrameTs>> camTs;
       for (int i = 0; i < kMaxCams; ++i) {
           auto ts = read_timestamps(videoDir.filePath(
               QString("timestamps_cam%1.csv").arg(i)));
           if (ts.isEmpty()) { break; }
           camTs.append(std::move(ts));
       }
   
       if (camTs.isEmpty()) {
           log_warning(QString("[SyncManifest] No timestamp files in %1").arg(videoDir.path()));
           return m;
       }
   
       const int nCams = static_cast<int>(camTs.size());
   
       // ── Common recording window, on the monotonic elapsed_ns clock ────────
       // t_origin = latest first-frame elapsed time  (all cameras are live at t_origin)
       // t_end    = earliest last-frame elapsed time (all cameras still live at t_end)
       int64_t tOrigin = std::numeric_limits<int64_t>::min();
       int64_t tEnd    = std::numeric_limits<int64_t>::max();
       for (int c = 0; c < nCams; ++c) {
           tOrigin = std::max(tOrigin, camTs[c].front().elapsedNs);
           tEnd    = std::min(tEnd,    camTs[c].back().elapsedNs);
       }
       // If cameras don't overlap (e.g. one stopped very early) fall back to union window.
       if (tEnd <= tOrigin) {
           tOrigin = std::numeric_limits<int64_t>::max();
           tEnd    = std::numeric_limits<int64_t>::min();
           for (int c = 0; c < nCams; ++c) {
               tOrigin = std::min(tOrigin, camTs[c].front().elapsedNs);
               tEnd    = std::max(tEnd,    camTs[c].back().elapsedNs);
           }
       }
       if (tEnd <= tOrigin) {
           log_warning("[SyncManifest] Cannot determine a valid time window.");
           return m;
       }
   
       m.tOriginNs_  = tOrigin;
       m.durationMs_ = (tEnd - tOrigin) / 1'000'000LL;
       m.totalTicks_ = static_cast<int>((tEnd - tOrigin) / m.stepNs_) + 1;
       m.totalTicks_ = std::max(m.totalTicks_, 1);
   
       // t_origin expressed on the wall clock too — same real-world instant,
       // computed independently in wall_ns space (same "latest first frame"
       // rule) since audio_seek_ms must be relative to session_start_utc.
       int64_t tOriginWall = std::numeric_limits<int64_t>::min();
       for (int c = 0; c < nCams; ++c) {
           tOriginWall = std::max(tOriginWall, camTs[c].front().wallNs);
       }
       m.tOriginWallNs_ = tOriginWall;
   
       // Audio seek: gap between session_start_utc and t_origin (wall clock).
       const int64_t sessionStartWallNs = session_start_wall_ns(sessionPath);
       if (sessionStartWallNs > 0 && tOriginWall > sessionStartWallNs) {
           m.audioSeekMs_ = (tOriginWall - sessionStartWallNs) / 1'000'000LL;
       }
   
       // ── Allocate columnar storage ─────────────────────────────────────────
       m.frameIds_.fill(-1, nCams * m.totalTicks_);
       m.deltaMs_.fill(0.0f, nCams * m.totalTicks_);
   
       // ── Per-camera nearest-neighbour frame assignment ─────────────────────
       const double halfStepMs = m.stepNs_ / 2.0 / 1e6;
   
       for (int c = 0; c < nCams; ++c) {
           const auto& frames = camTs[c];
           const int   nf     = static_cast<int>(frames.size());
           int ptr = 0;
   
           double sumAbsDelta = 0.0;
           double maxAbsDelta = 0.0;
           int    freshTicks  = 0;
   
           for (int tick = 0; tick < m.totalTicks_; ++tick) {
               const int64_t tIdeal = tOrigin + static_cast<int64_t>(tick) * m.stepNs_;
   
               // Advance ptr while the next frame is closer to tIdeal.
               while (ptr + 1 < nf) {
                   const double dCur  = std::abs(static_cast<double>(frames[ptr].elapsedNs     - tIdeal));
                   const double dNext = std::abs(static_cast<double>(frames[ptr + 1].elapsedNs - tIdeal));
                   if (dNext < dCur) { ++ptr; } else { break; }
               }
   
               const double delta = static_cast<double>(frames[ptr].elapsedNs - tIdeal) / 1e6;
               const int    base  = c * m.totalTicks_ + tick;
               m.frameIds_[base]  = frames[ptr].frameId;
               m.deltaMs_[base]   = static_cast<float>(delta);
   
               const double absDelta = std::abs(delta);
               sumAbsDelta += absDelta;
               if (absDelta > maxAbsDelta) { maxAbsDelta = absDelta; }
               if (absDelta <= halfStepMs) { ++freshTicks; }
           }
   
           CameraSync cs;
           cs.index          = c;
           cs.videoFile      = QString("video/video_%1.mp4").arg(c);
           cs.framesCaptured = nf;
           cs.firstWallNs    = frames.front().wallNs;
           cs.lastWallNs     = frames.back().wallNs;
           // How far into the video the player must seek to reach master t = 0.
           // The mp4's own PTS timeline is zeroed at this camera's first
           // *elapsed_ns*-stamped frame (see VideoEncoder), so the seek offset
           // must be computed in the same clock to land on the right frame.
           cs.seekOffsetMs   = (tOrigin - frames.front().elapsedNs) / 1'000'000LL;
           // Actual fps is a duration measurement — use the monotonic clock,
           // per elapsed_ns()'s own contract, rather than wall_ns which can
           // jump under an NTP adjustment mid-session.
           cs.fpsActual      = (nf > 1)
               ? static_cast<double>(nf - 1) * 1e9 /
                 static_cast<double>(frames.back().elapsedNs - frames.front().elapsedNs)
               : 0.0;
           cs.coveragePct    = 100.0 * freshTicks / m.totalTicks_;
           cs.meanDeltaMs    = m.totalTicks_ > 0 ? sumAbsDelta / m.totalTicks_ : 0.0;
           cs.maxDeltaMs     = maxAbsDelta;
           m.cameras_.append(cs);
       }
   
       m.generatedAt_ = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
   
       log_info(QString("[SyncManifest] Generated — %1 cams  %2 ticks  %3 ms  "
                        "master %.1f fps  audio_seek %4 ms")
                    .arg(nCams).arg(m.totalTicks_).arg(m.durationMs_)
                    .arg(masterFps)
                    .arg(m.audioSeekMs_));
   
       return m;
   }
   
   // ── save ─────────────────────────────────────────────────────────────────
   
   bool SyncManifest::save(const QString& sessionPath) const
   {
       QJsonObject root;
       root["schema"]           = "mosaic-sync-v2";
       root["session_path"]     = sessionPath;
       root["generated_at_utc"] = generatedAt_;
       root["t_origin_ns"]      = QString::number(tOriginNs_);
       root["t_origin_wall_ns"] = QString::number(tOriginWallNs_);
       root["duration_ms"]      = durationMs_;
       root["master_fps"]       = masterFps_;
       root["step_ns"]          = QString::number(stepNs_);
       root["total_ticks"]      = totalTicks_;
       root["audio_seek_ms"]    = audioSeekMs_;
   
       QJsonArray cams;
       for (const auto& cs : cameras_) {
           QJsonObject o;
           o["index"]           = cs.index;
           o["video_file"]      = cs.videoFile;
           o["frames_captured"] = cs.framesCaptured;
           o["fps_actual"]      = cs.fpsActual;
           o["coverage_pct"]    = cs.coveragePct;
           o["mean_delta_ms"]   = cs.meanDeltaMs;
           o["max_delta_ms"]    = cs.maxDeltaMs;
           o["first_wall_ns"]   = QString::number(cs.firstWallNs);
           o["last_wall_ns"]    = QString::number(cs.lastWallNs);
           o["seek_offset_ms"]  = cs.seekOffsetMs;
           cams.append(o);
       }
       root["cameras"] = cams;
   
       // Columnar tick data: one int array and one float array per camera.
       QJsonObject ticks;
       const int nCams = static_cast<int>(cameras_.size());
       for (int c = 0; c < nCams; ++c) {
           QJsonArray ids, deltas;
           for (int t = 0; t < totalTicks_; ++t) {
               ids.append(frameIds_[c * totalTicks_ + t]);
               deltas.append(static_cast<double>(
                   std::round(deltaMs_[c * totalTicks_ + t] * 10.0) / 10.0));
           }
           ticks[QString("cam%1_frame_ids").arg(c)] = ids;
           ticks[QString("cam%1_delta_ms").arg(c)]  = deltas;
       }
       root["ticks"] = ticks;
   
       const QString outPath = QDir(sessionPath).filePath("sync_manifest.json");
       QFile f(outPath);
       if (!f.open(QIODevice::WriteOnly)) {
           log_warning("[SyncManifest] Cannot write " + outPath);
           return false;
       }
       f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
       log_info("[SyncManifest] Saved → " + outPath);
       return true;
   }
   
   // ── load ─────────────────────────────────────────────────────────────────
   
   SyncManifest SyncManifest::load(const QString& sessionPath)
   {
       SyncManifest m;
       QFile f(QDir(sessionPath).filePath("sync_manifest.json"));
       if (!f.open(QIODevice::ReadOnly)) { return m; }
   
       const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
       if (root.isEmpty()) { return m; }
   
       m.generatedAt_    = root["generated_at_utc"].toString();
       m.tOriginNs_      = root["t_origin_ns"].toString().toLongLong();
       m.tOriginWallNs_  = root["t_origin_wall_ns"].toString().toLongLong();
       m.durationMs_     = root["duration_ms"].toInteger();
       m.masterFps_      = root["master_fps"].toDouble(25.0);
       m.stepNs_         = root["step_ns"].toString().toLongLong();
       m.totalTicks_     = root["total_ticks"].toInt();
       m.audioSeekMs_    = root["audio_seek_ms"].toInteger();
   
       for (const auto& cv : root["cameras"].toArray()) {
           const QJsonObject o = cv.toObject();
           CameraSync cs;
           cs.index          = o["index"].toInt();
           cs.videoFile      = o["video_file"].toString();
           cs.framesCaptured = o["frames_captured"].toInt();
           cs.fpsActual      = o["fps_actual"].toDouble();
           cs.coveragePct    = o["coverage_pct"].toDouble();
           cs.meanDeltaMs    = o["mean_delta_ms"].toDouble();
           cs.maxDeltaMs     = o["max_delta_ms"].toDouble();
           cs.firstWallNs    = o["first_wall_ns"].toString().toLongLong();
           cs.lastWallNs     = o["last_wall_ns"].toString().toLongLong();
           cs.seekOffsetMs   = o["seek_offset_ms"].toInteger();
           m.cameras_.append(cs);
       }
   
       const int nCams = static_cast<int>(m.cameras_.size());
       if (nCams == 0 || m.totalTicks_ == 0) { return m; }
   
       m.frameIds_.fill(-1, nCams * m.totalTicks_);
       m.deltaMs_.fill(0.0f, nCams * m.totalTicks_);
   
       const QJsonObject ticks = root["ticks"].toObject();
       for (int c = 0; c < nCams; ++c) {
           const QJsonArray ids    = ticks[QString("cam%1_frame_ids").arg(c)].toArray();
           const QJsonArray deltas = ticks[QString("cam%1_delta_ms").arg(c)].toArray();
           const int        count  = std::min({static_cast<int>(ids.size()),
                                                static_cast<int>(deltas.size()),
                                                m.totalTicks_});
           for (int t = 0; t < count; ++t) {
               m.frameIds_[c * m.totalTicks_ + t] = ids[t].toInt(-1);
               m.deltaMs_ [c * m.totalTicks_ + t] = static_cast<float>(
                   deltas[t].toDouble());
           }
       }
       return m;
   }
   
   // ── queries ───────────────────────────────────────────────────────────────
   
   bool SyncManifest::is_valid() const
   {
       return !cameras_.isEmpty() && totalTicks_ > 0 && stepNs_ > 0;
   }
   
   int     SyncManifest::camera_count()     const { return static_cast<int>(cameras_.size()); }
   int     SyncManifest::total_ticks()      const { return totalTicks_; }
   double  SyncManifest::master_fps()       const { return masterFps_; }
   int64_t SyncManifest::duration_ms()      const { return durationMs_; }
   int64_t SyncManifest::t_origin_ns()      const { return tOriginNs_; }
   int64_t SyncManifest::t_origin_wall_ns() const { return tOriginWallNs_; }
   int64_t SyncManifest::audio_seek_ms()    const { return audioSeekMs_; }
   
   static CameraSync kEmptyCamera;
   const CameraSync& SyncManifest::camera_info(int idx) const
   {
       if (idx < 0 || idx >= cameras_.size()) { return kEmptyCamera; }
       return cameras_[idx];
   }
   
   int SyncManifest::frame_at_tick(int cameraIdx, int tick) const
   {
       if (!is_valid()) { return -1; }
       if (cameraIdx < 0 || cameraIdx >= cameras_.size()) { return -1; }
       if (tick < 0 || tick >= totalTicks_) { return -1; }
       return frameIds_[cameraIdx * totalTicks_ + tick];
   }
   
   double SyncManifest::delta_ms_at_tick(int cameraIdx, int tick) const
   {
       if (!is_valid()) { return 0.0; }
       if (cameraIdx < 0 || cameraIdx >= cameras_.size()) { return 0.0; }
       if (tick < 0 || tick >= totalTicks_) { return 0.0; }
       return static_cast<double>(deltaMs_[cameraIdx * totalTicks_ + tick]);
   }
   
   int SyncManifest::tick_for_ms(int64_t posMs) const
   {
       if (stepNs_ == 0) { return 0; }
       const int tick = static_cast<int>(posMs * 1'000'000LL / stepNs_);
       return std::clamp(tick, 0, totalTicks_ - 1);
   }
   
   int64_t SyncManifest::seek_offset_ms(int cameraIdx) const
   {
       if (cameraIdx < 0 || cameraIdx >= cameras_.size()) { return 0; }
       return cameras_[cameraIdx].seekOffsetMs;
   }
   
   QString SyncManifest::quality_report() const
   {
       if (!is_valid()) { return "No sync manifest."; }
       QString r;
       r += QString("Duration %1 ms   master %.1f fps   %2 ticks\n\n")
                .arg(durationMs_).arg(masterFps_).arg(totalTicks_);
       for (const auto& cs : cameras_) {
           const char* grade = cs.coveragePct >= 80 ? "OK"
                             : cs.coveragePct >= 50 ? "LOW"
                                                    : "POOR";
           r += QString("  Cam %1  %2 frames  %.1f fps  %.0f%% coverage [%3]"
                        "  mean Δ %.1f ms  max Δ %.1f ms\n")
                    .arg(cs.index)
                    .arg(cs.framesCaptured)
                    .arg(cs.fpsActual)
                    .arg(cs.coveragePct)
                    .arg(grade)
                    .arg(cs.meanDeltaMs)
                    .arg(cs.maxDeltaMs);
       }
       return r;
   }
   
   } // namespace mosaic
