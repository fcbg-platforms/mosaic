C++ API Reference
=====================

.. contents:: On this page
   :local:
   :depth: 2

MOSAIC's C++ layer owns the UI, hardware I/O (cameras, microphones, trigger
lines), and orchestration — starting/stopping recordings, launching Python
analysis subprocesses, and rendering results. The heavy lifting for machine
learning and signal processing lives in the separate ``analysis/`` Python
project (see :doc:`analysis_api`) instead. The bridge between the two is a
small family of **result classes** — plain C++ structures that parse the
JSON a Python plugin wrote and expose it to Qt for drawing overlays and
charts; see :ref:`cpp-api-results` below.

This page is a curated, hand-grouped tour of the classes you're actually
likely to want — organized by subsystem, each with a one-sentence
description. For the full, alphabetical, auto-generated listing of every
documented class, struct, function, and file (useful for exhaustive
browsing or following a specific cross-reference), see
:doc:`api/library_root`.

.. grid:: 2 2 3 4
   :gutter: 3

   .. grid-item-card:: 🧭 Core & Application
      :link: cpp-api-core
      :link-type: ref

      The app object and process-wide services: startup wiring, profiles,
      logging.

   .. grid-item-card:: 🎬 Recording
      :link: cpp-api-recording
      :link-type: ref

      Session coordination and cross-camera timestamp bookkeeping.

   .. grid-item-card:: 📹 Video
      :link: cpp-api-video
      :link-type: ref

      Per-camera capture, encoding, and the GigE Action-Command trigger.

   .. grid-item-card:: 🎙 Audio
      :link: cpp-api-audio
      :link-type: ref

      Per-microphone capture and WAV writing.

   .. grid-item-card:: 📐 Calibration & Room
      :link: cpp-api-calibration
      :link-type: ref

      Intrinsic checkerboard calibration and multi-camera extrinsic
      ("room") calibration.

   .. grid-item-card:: ⚡ Trigger & Sync
      :link: cpp-api-trigger
      :link-type: ref

      Keyboard/serial/parallel-port trigger sources and post-hoc
      trigger-to-frame resolution.

   .. grid-item-card:: 🧠 Analysis orchestration
      :link: cpp-api-orchestration
      :link-type: ref

      Launching Python analysis jobs and the Real-time tab's live
      subprocess workers.

   .. grid-item-card:: 🔗 Analysis result types
      :link: cpp-api-results
      :link-type: ref

      The Python ↔ C++ bridge — one loader class per analysis plugin's
      JSON output.

.. _cpp-api-core:

Core & Application
-----------------------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Class
     - What it does
   * - :cpp:class:`mosaic::Application`
     - Top-level application object — owns and wires together every
       subsystem manager (video, audio, trigger, record, analysis) at
       startup.
   * - :cpp:class:`mosaic::ProfileManager`
     - Manages the on-disk research-group profile manifest and each
       profile's isolated settings directory. See :doc:`profiles`.
   * - :cpp:class:`mosaic::Logger`
     - File-based logger with severity levels, backing ``mosaic.log``.

.. _cpp-api-recording:

Recording
-------------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Class
     - What it does
   * - :cpp:class:`mosaic::RecordManager`
     - Central coordinator for a recording session — creates the session
       folder, writes ``session_meta.json`` immediately on start, and
       starts/stops the trigger, audio, and video pipelines in the
       correct order. See :doc:`recording`.
   * - :cpp:class:`mosaic::SyncManifest`
     - Resolves every camera's frame timestamps onto one shared, uniform
       master-tick grid, for frame-accurate synchronized multi-camera
       playback.
   * - :cpp:class:`mosaic::FrameTimestampWriter`
     - Writes one CSV row per grabbed frame (``frame_id``, ``elapsed_ns``,
       ``wall_ns``, ``hw_timestamp_ns``) that downstream sync tools read.
   * - :cpp:class:`mosaic::RingBuffer`
     - Lock-free single-producer/single-consumer ring buffer handing
       frames from the grab thread to the encoder thread.

.. _cpp-api-video:

Video
---------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Class
     - What it does
   * - :cpp:class:`mosaic::VideoManager`
     - Orchestrates one :cpp:class:`~mosaic::VideoGrabber` +
       :cpp:class:`~mosaic::VideoEncoder` pair per configured camera,
       including arming and firing the shared GigE Action-Command trigger.
   * - :cpp:class:`mosaic::VideoGrabber`
     - Grabs frames from one camera (Basler Pylon SDK, or a test-pattern
       stub when no camera hardware is enabled) and pushes them into a
       shared ring buffer.
   * - :cpp:class:`mosaic::VideoEncoder`
     - Consumes frames from a ring buffer and encodes them to an MP4 file
       — NVENC, VideoToolbox, or a ``libx264`` software fallback depending
       on what's available at compile time.
   * - :cpp:class:`mosaic::VideoFeedProvider`
     - ``QQuickImageProvider`` that serves the latest live camera frame to
       the QML monitor view.
   * - :cpp:class:`mosaic::ActionCommandSession`
     - Owns the GigE transport-layer handle used to fire continuous
       per-frame Action-Command triggers across every armed camera.

.. _cpp-api-audio:

Audio
---------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Class
     - What it does
   * - :cpp:class:`mosaic::AudioManager`
     - Coordinates all :cpp:class:`~mosaic::AudioRecorder` instances for a
       recording session.
   * - :cpp:class:`mosaic::AudioRecorder`
     - Records one configured microphone to a WAV file and emits live
       RMS/envelope signals that drive the waveform and VU-meter displays.
   * - :cpp:class:`mosaic::WavWriter`
     - Writes PCM audio to a WAV file, fixing up the RIFF/``data`` chunk
       sizes when closed.

.. _cpp-api-calibration:

Calibration & Room
-----------------------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Class
     - What it does
   * - :cpp:class:`mosaic::CalibrationManager`
     - Runs a full single-camera checkerboard intrinsic calibration
       pipeline using OpenCV. See :doc:`calibration`.
   * - :cpp:class:`mosaic::RoomCalibrationManager`
     - Solves multi-camera extrinsic ("room") calibration from a shared
       ChArUco board seen by several cameras at once — see
       :doc:`math/room_calibration` for the underlying pose-graph math.

.. _cpp-api-trigger:

Trigger & Sync
------------------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Class
     - What it does
   * - :cpp:class:`mosaic::TriggerManager`
     - Central trigger coordinator — aggregates events from every
       configured trigger source and logs them.
   * - :cpp:class:`mosaic::TriggerRecorder`
     - Writes :cpp:struct:`~mosaic::TriggerEvent`\ s to ``trigger.csv``
       during a recording session; thread-safe.
   * - :cpp:class:`mosaic::KeyboardTrigger`
     - Fires a trigger event on a configured key binding, installed as an
       app-wide event filter.
   * - :cpp:class:`mosaic::SerialTrigger`
     - Receives trigger events from an RS-232/USB-serial port.
   * - :cpp:class:`mosaic::ParallelPortTrigger`
     - Polls a parallel port's Data register for incoming TTL trigger
       pulses (e.g. from an EEG amplifier), and can drive the Control
       register's INIT pin to mark recording start/stop back to that same
       equipment.
   * - :cpp:class:`mosaic::TriggerFrameMap`
     - Resolves every ``trigger.csv`` event to the nearest
       actually-captured frame in every camera, for the EEG/Trigger ↔
       Frame Sync Analysis-tab plugin.

.. _cpp-api-orchestration:

Analysis orchestration
---------------------------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Class
     - What it does
   * - :cpp:class:`mosaic::AnalysisManager`
     - Manages the Python analysis subprocess(es) for every post-recording
       Analysis-tab plugin (Pose, Face Masking, Diarization, ...).
   * - :cpp:class:`mosaic::PoseWorker`
     - Owns the long-lived live pose/gaze estimation subprocess behind the
       Real-time tab's camera tiles.
   * - :cpp:class:`mosaic::TranscriptWorker`
     - Owns the long-lived live speech-to-text subprocess behind the
       Real-time tab's caption panel. See
       :ref:`analysis-api-transcribe` for its Python counterpart.
   * - :cpp:class:`mosaic::DetectionRateTracker`
     - Buckets a stream of detected/not-detected observations into fixed
       time windows, for the Real-time tab's per-camera detection-rate
       sparkline.

.. _cpp-api-results:

Analysis result types — the Python ↔ C++ bridge
-----------------------------------------------------

Every post-hoc Analysis-tab plugin follows the same shape: a Python script
under ``analysis/`` writes a JSON file, and one of these classes loads it
back into a queryable, typed C++ structure for the Analysis tab to draw an
overlay or chart from. Each row below links to the Python plugin section
that actually produces the file it loads.

.. list-table::
   :header-rows: 1
   :widths: 25 55 20

   * - Class
     - What it does
     - Written by
   * - :cpp:class:`mosaic::PoseAnalysisResult`
     - Loads a ``pose/<video>.<model>.pose.json`` sidecar for the Pose
       plugin's skeleton overlay and kinematics chart.
     - :doc:`analysis_api`
   * - :cpp:class:`mosaic::ExpressionResult`
     - Loads an ``expression/<video>.expression.json`` sidecar for the
       Facial Expression plugin's overlay and blendshape/Action-Unit
       charts.
     - :doc:`analysis_api`
   * - :cpp:class:`mosaic::GazeFusionResult`
     - Loads a session-root ``gaze_fusion.json`` for the Multi-Camera
       Gaze Fusion plugin's per-camera overlay and 3D room view.
     - :doc:`analysis_api`
   * - :cpp:class:`mosaic::TranscriptResult`
     - Loads an ``audio/<mic>.transcript.json`` sidecar for the Speaker
       Diarization plugin's transcript table and speaker-shaded waveform.
     - :doc:`analysis_api`
   * - :cpp:class:`mosaic::RppgResult`
     - Loads a ``rppg/<video>.<backend>.rppg.json`` sidecar for the
       **experimental** Remote Heart Rate plugin's BPM chart and debug
       ROI overlay.
     - :doc:`analysis_api`
   * - :cpp:class:`mosaic::Skeleton3DResult`
     - Loads a session-root ``skeleton3d.json`` for the 3D Pose
       Reconstruction plugin's reprojected overlay and interactive 3D
       room view.
     - :doc:`analysis_api`

----

Full generated API index
-----------------------------

The curated tour above covers the classes you're most likely to look for.
For everything else — every documented function, struct, enum, and
header, browsable alphabetically or by file — see the full
Doxygen/Breathe/Exhale-generated reference:

.. grid:: 1
   :gutter: 2

   .. grid-item-card:: 📖 Full API index
      :link: api/library_root
      :link-type: doc

      Every documented C++ entity in ``src/`` (excluding the Qt/QML UI
      layer), auto-generated from source on every build. Regenerates from
      scratch each time — never hand-edited.

.. toctree::
   :hidden:

   api/library_root
