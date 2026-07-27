
.. _program_listing_file_src_analysis_analysis_manager.hpp:

Program Listing for File analysis_manager.hpp
=============================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_analysis_analysis_manager.hpp>` (``src\analysis\analysis_manager.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <QObject>
   #include <QProcessEnvironment>
   #include <QString>
   #include <QStringList>
   #include <memory>
   
   namespace mosaic {
   
   /// @brief Manages the Python analysis subprocess(es) for post-recording plugins
   /// (pose estimation, face masking, speaker diarization/transcription, facial
   /// expression).
   ///
   /// analyze_session() / run_face_mask() / run_diarization() /
   /// run_expression_analysis() always run when called directly (e.g. a UI "Run"
   /// button). @ref auto_analyze is a plain flag
   /// callers can check to
   /// decide whether to also trigger analysis automatically when a recording
   /// session ends — AnalysisManager itself does not gate on it. Only one script
   /// runs at a time regardless of which plugin queued it; a second call while
   /// one is running queues behind it.
   ///
   /// The Python script is found relative to the application's executable directory
   /// (for installed builds) or the project root (during development).
   ///
   /// @par Usage
   /// @code{.cpp}
   /// AnalysisManager mgr;
   /// connect(&mgr, &AnalysisManager::output_received, this, [](const QString& line) {
   ///     log_info("[Analysis] " + line);
   /// });
   /// connect(recordMgr, &RecordManager::recording_stopped, &mgr,
   ///         [&](const QString& path, int) {
   ///             if (mgr.auto_analyze()) { mgr.analyze_session(path); }
   ///         });
   /// mgr.set_auto_analyze(true);
   /// @endcode
   ///
   /// @see RecordManager
   class AnalysisManager : public QObject {
       Q_OBJECT
   public:
       explicit AnalysisManager(QObject* parent = nullptr);
       ~AnalysisManager() override;
   
       // ── Configuration ───────────────────────────────────────────────────────
   
       /// @brief Enable / disable automatic post-recording analysis.
       ///
       /// When @c true, @ref analyze_session() is triggered automatically by
       /// the @c recording_stopped connection in Application.
       void set_auto_analyze(bool enabled);
   
       /// @returns @c true if auto-analysis is enabled.
       [[nodiscard]] bool auto_analyze() const;
   
       /// @brief Override the Python interpreter path.
       ///
       /// By default, the manager searches for a virtual-environment interpreter at
       /// @c analysis/.venv/bin/python (macOS/Linux) or @c analysis\.venv\Scripts\python.exe
       /// (Windows), then falls back to the system @c python3 / @c python.
       ///
       /// @param path  Absolute path to the Python executable.
       void set_python_path(const QString& path);
   
       /// @brief Set the YOLOv8 model variant.
       ///
       /// @param modelName  e.g. @c "yolov8n-pose.pt" (default) or @c "yolov8s-pose.pt".
       void set_model(const QString& modelName);
   
       /// @brief Set how many frames to skip between pose estimates.
       ///
       /// @c 1 = every frame (slowest, most detailed).
       /// @c 5 = every 5th frame (≈6 fps at 30 fps recording).
       void set_frame_skip(int skip);
   
       // ── Status ───────────────────────────────────────────────────────────────
   
       /// @returns @c true while the analysis subprocess is running.
       [[nodiscard]] bool is_running() const;
   
       // ── Operations ───────────────────────────────────────────────────────────
   
       /// @brief Analyse all .mp4 files in @p sessionPath asynchronously.
       ///
       /// Always runs when called directly (e.g. from a UI "Run" action).
       /// @ref auto_analyze() only gates whether the caller triggers this
       /// automatically after a recording stops — it is not checked here.
       /// If a previous analysis is still running, this queues the new session.
       ///
       /// @param sessionPath  Absolute path to the recorded session directory.
       void analyze_session(const QString& sessionPath);
   
       /// @brief Anonymize (blur/box) faces in all .mp4 files in @p sessionPath,
       /// writing output into a sibling "anonymized/" folder — originals are
       /// never modified.
       ///
       /// Always runs when called directly, exactly like analyze_session(). If a
       /// previous analysis is still running, this queues the new job.
       ///
       /// @param sessionPath  Absolute path to the recorded session directory.
       /// @param backend      "mediapipe" (default), "yolov8", or "opencv".
       /// @param style        "blur" (default) or "box".
       /// @param frameSkip    Run the detector every Nth frame, reusing the last
       ///                     detected boxes on skipped frames. Defaults to 1
       ///                     (every frame) at the call site — raising this
       ///                     risks a skipped frame's fast head motion going
       ///                     unmasked.
       void run_face_mask(const QString& sessionPath, const QString& backend,
                           const QString& style, int frameSkip);
   
       /// @brief Transcribe (and, when possible, diarize) all .wav files in
       /// @p sessionPath, writing a "<name>.transcript.json" sidecar next to
       /// each one — originals are never modified.
       ///
       /// Always runs when called directly, exactly like analyze_session(). If a
       /// previous analysis is still running, this queues the new job.
       ///
       /// @param sessionPath    Absolute path to the recorded session directory.
       /// @param modelSize      faster-whisper model size, e.g. "small" (default).
       /// @param language       Force a language code (e.g. "en"), or empty to auto-detect.
       /// @param hfToken        Hugging Face access token for the gated pyannote
       ///                       diarization models, or empty to skip diarization
       ///                       (transcript-only output).
       /// @param minSpeakers    Optional pyannote hint, 0 = unset.
       /// @param maxSpeakers    Optional pyannote hint, 0 = unset.
       /// @param skipDiarization  Force transcript-only even if hfToken is set.
       void run_diarization(const QString& sessionPath, const QString& modelSize,
                             const QString& language, const QString& hfToken,
                             int minSpeakers, int maxSpeakers, bool skipDiarization);
   
       /// @brief Detect faces and classify a dominant basic-emotion label per
       /// face in all .mp4 files in @p sessionPath, writing a
       /// "<name>.expression.json" sidecar next to each one — originals are
       /// never modified.
       ///
       /// Always runs when called directly, exactly like analyze_session(). If a
       /// previous analysis is still running, this queues the new job.
       ///
       /// @param sessionPath     Absolute path to the recorded session directory.
       /// @param backend         "heuristic" (default, rule-based blendshapes)
       ///                        or "ferplus" (pretrained FER+ ONNX model).
       /// @param maxFaces        Maximum simultaneous faces to detect per frame.
       /// @param minConfidence   Face detection/presence confidence threshold (0-1).
       /// @param frameSkip       Process every Nth frame (1 = every frame).
       void run_expression_analysis(const QString& sessionPath, const QString& backend,
                                     int maxFaces, double minConfidence, int frameSkip);
   
       /// @brief Fuse per-camera 3D gaze rays (from every camera with both
       /// intrinsic and extrinsic calibration) into a triangulated room-space
       /// gaze origin/direction, plus (when the room plane is defined) a
       /// target point, per synchronized master tick. Writes a session-root
       /// "gaze_fusion.json" sidecar — originals are never modified.
       ///
       /// Always runs when called directly, exactly like analyze_session(). If a
       /// previous analysis is still running, this queues the new job.
       ///
       /// Proactively generates+saves sync_manifest.json first if the session
       /// doesn't already have one (mirrors SessionPlayerW's own "generate if
       /// missing" pattern) — the fusion script requires it and shouldn't have
       /// to duplicate that C++-side generation logic in Python.
       ///
       /// @param sessionPath    Absolute path to the recorded session directory.
       /// @param minCameras     Minimum simultaneous cameras required to compute
       ///                       a target point (rays are still recorded below this).
       /// @param minConfidence  Face detection/presence confidence threshold (0-1).
       /// @param frameSkip      Process every Nth frame per camera (1 = every frame).
       void run_gaze_fusion(const QString& sessionPath, int minCameras,
                             double minConfidence, int frameSkip);
   
       /// @brief Stop the currently running analysis process immediately.
       void stop();
   
   signals:
       /// Emitted for every line of stdout / stderr from the Python subprocess.
       void output_received(QString line);
   
       /// Emitted when the subprocess starts.
       void analysis_started(QString sessionPath);
   
       /// Emitted when the subprocess exits.
       void analysis_finished(QString sessionPath, bool success);
   
       /// Emitted if the Python interpreter or script cannot be found.
       void setup_error(QString message);
   
   private slots:
       void on_stdout_ready();
       void on_stderr_ready();
       void on_process_finished(int exitCode, int exitStatus);
   
   private:
       [[nodiscard]] QString find_python()     const;
       [[nodiscard]] QString find_script(const QString& scriptRelPath) const;
       [[nodiscard]] QString find_venv_python() const;
       void enqueue_or_launch(const QString& sessionPath, const QString& scriptRelPath,
                               const QStringList& args, const QProcessEnvironment& env);
       void launch(const QString& sessionPath, const QString& scriptRelPath,
                   const QStringList& args, const QProcessEnvironment& env);
   
       struct Impl;
       std::unique_ptr<Impl> d;
   };
   
   } // namespace mosaic
