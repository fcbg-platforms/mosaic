Architecture
============

.. contents:: On this page
   :local:
   :depth: 2

Overview
--------

MOSAIC is built on **Qt 6 / C++23** and organised into five layers:

.. code-block:: text

   ┌──────────────────────────────────────────────────────────────────┐
   │  UI  (Qt Widgets + QML)                                          │
   │  VideoSettingsW  AudioSettingsW  TriggerSettingsW  CalibrationW  │
   │  LoggerPanelW    LoginDialog     MonitorView.qml                 │
   ├──────────────────────────────────────────────────────────────────┤
   │  Managers  (main-thread coordinators)                            │
   │  VideoManager   AudioManager   TriggerManager   RecordManager    │
   ├──────────────────────────────────────────────────────────────────┤
   │  Workers  (background threads)                                   │
   │  VideoGrabber   VideoEncoder   LslInlet   (audio: QAudioSource)  │
   ├──────────────────────────────────────────────────────────────────┤
   │  Core  (settings, auth, logging)                                 │
   │  Application   AppSettings   ProfileManager   Logger             │
   ├──────────────────────────────────────────────────────────────────┤
   │  Utils  (header-only, no Qt dependency)                          │
   │  RingBuffer<T>   elapsed_ns()   wall_clock_ns()                  │
   └──────────────────────────────────────────────────────────────────┘

Startup sequence
----------------

.. code-block:: text

   main()
     └─ ProfileManager::load()
     └─ LoginDialog::exec()          ← blocks until a profile is chosen
     └─ Application::initialize(username)
           ├─ Logger::open_log_file()
           ├─ AppSettings::load()    ← reads ~/.config/CSRU/mosaic/profiles/<user>/settings.json
           ├─ TriggerManager()       ← installs keyboard event filters immediately
           ├─ AudioManager()
           ├─ VideoManager::open()   ← opens cameras (or stubs)
           ├─ RecordManager()
           └─ MainWindow::show()

Video pipeline
--------------

Each configured camera runs its own pair of threads:

.. code-block:: text

   Camera (Pylon)                  No camera
       │                               │
       ▼                               ▼
   VideoGrabber (QThread)     VideoGrabber stub (test-pattern generator)
       │  pushes VideoFrame
       ▼
   RingBuffer<shared_ptr<VideoFrame>>   capacity = 128 frames
       │  pops VideoFrame
       ▼
   VideoEncoder (QThread)
       ├─ FFmpeg/NVENC → video_N.mp4
       └─ FrameTimestampWriter → timestamps_camN.csv

   VideoManager owns all (Grabber, Buffer, Encoder) triples and
   starts / stops them together via RecordManager.

Thread safety
~~~~~~~~~~~~~

- ``RingBuffer<T>`` is a **lock-free SPSC** structure (one producer, one consumer).
  Each camera has its own ring so there is no cross-camera contention.
- ``FrameTimestampWriter::write()`` is mutex-protected (called from grabber thread,
  ``stop()`` from main thread).
- ``Logger::log()`` is mutex-protected; the ``entry_added`` signal is emitted
  from the calling thread — connect with ``Qt::QueuedConnection`` when the
  receiver lives on the main thread.

Session output
--------------

A recording session creates the following files:

.. code-block:: text

   recordings/
   └── 2026-06-04_14-32-05/
       ├── session_meta.json        ← cameras, mics, triggers, recorded_by, start UTC
       ├── video_0.mp4              ── one per camera
       ├── timestamps_cam0.csv      ── frame_id, elapsed_ns, wall_ns, hw_timestamp_ns  (per camera)
       ├── audio_0.wav              ── one per microphone
       └── trigger.csv             ── elapsed_ms, wall_clock, source, label, value

Settings persistence
--------------------

Settings are stored as JSON (schema v1) in a per-profile directory:

.. code-block:: text

   ~/.config/CSRU/mosaic/
     profiles.json                  ← auth manifest (hashed passwords)
     profiles/
       <username>/
         settings.json              ← full AppSettings (cameras, audio, triggers, record)
         mosaic.log

Switching profiles (``File → Switch profile``) exits with code **42**; ``main()``
detects this and re-shows the login dialog, rebuilding the entire
``Application`` + ``MainWindow`` with the new user's settings.

Feature-flag guards
-------------------

Hardware subsystems are compiled under preprocessor guards so the software
runs on any developer machine without lab hardware:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Guard
     - What it enables
   * - ``MOSAIC_HAVE_CAMERAS``
     - Basler Pylon camera access in ``VideoGrabber``
   * - ``MOSAIC_HAVE_FFMPEG``
     - FFmpeg encode path in ``VideoEncoder``
   * - ``MOSAIC_HAVE_NVENC``
     - NVIDIA NVENC codec selection
   * - ``MOSAIC_HAVE_VIDEOTOOLBOX``
     - Apple VideoToolbox codec selection
   * - ``MOSAIC_HAVE_LSL``
     - ``LslOutlet`` / ``LslInlet`` using liblsl
   * - ``MOSAIC_HAVE_OPENCV``
     - Calibration in ``CalibrationManager``
   * - ``MOSAIC_HAVE_PARALLEL_PORT``
     - InpOut32 polling in ``ParallelPortTrigger``
