Architecture
============

.. contents:: On this page
   :local:
   :depth: 2

Overview
--------

MOSAIC is built on **Qt 6 / C++23** and organised into five layers:

.. raw:: html

   <div class="mosaic-diagram mosaic-stack">
     <div class="mosaic-layer ui">
       <div class="mosaic-layer-title">UI<span class="mosaic-layer-sub">Qt Widgets + QML</span></div>
       <div class="mosaic-layer-items">VideoSettingsW · AudioSettingsW · TriggerSettingsW · CalibrationW · LoggerPanelW · LoginDialog · MonitorView.qml</div>
     </div>
     <div class="mosaic-layer mgr">
       <div class="mosaic-layer-title">Managers<span class="mosaic-layer-sub">main-thread coordinators</span></div>
       <div class="mosaic-layer-items">VideoManager · AudioManager · TriggerManager · RecordManager</div>
     </div>
     <div class="mosaic-layer worker">
       <div class="mosaic-layer-title">Workers<span class="mosaic-layer-sub">background threads</span></div>
       <div class="mosaic-layer-items">VideoGrabber · VideoEncoder · ParallelPortTrigger · (audio: QAudioSource)</div>
     </div>
     <div class="mosaic-layer core">
       <div class="mosaic-layer-title">Core<span class="mosaic-layer-sub">settings, auth, logging</span></div>
       <div class="mosaic-layer-items">Application · AppSettings · ProfileManager · Logger</div>
     </div>
     <div class="mosaic-layer utils">
       <div class="mosaic-layer-title">Utils<span class="mosaic-layer-sub">header-only, no Qt dependency</span></div>
       <div class="mosaic-layer-items">RingBuffer&lt;T&gt; · elapsed_ns() · wall_clock_ns()</div>
     </div>
   </div>

Startup sequence
----------------

.. raw:: html

   <div class="mosaic-diagram mosaic-steps">
     <div class="mosaic-step">
       <div class="mosaic-step-dot">1</div>
       <div class="mosaic-step-body">
         <div class="mosaic-step-title">ProfileManager::load()</div>
         <div class="mosaic-step-desc">Reads the auth manifest of known profiles.</div>
       </div>
     </div>
     <div class="mosaic-step">
       <div class="mosaic-step-dot">2</div>
       <div class="mosaic-step-body">
         <div class="mosaic-step-title">LoginDialog::exec()</div>
         <div class="mosaic-step-desc">Blocks until a profile is chosen.</div>
       </div>
     </div>
     <div class="mosaic-step">
       <div class="mosaic-step-dot">3</div>
       <div class="mosaic-step-body">
         <div class="mosaic-step-title">Application::initialize(username)</div>
         <div class="mosaic-step-desc">
           <code>Logger::open_log_file()</code> →
           <code>AppSettings::load()</code> (reads
           <code>~/.config/CSRU/mosaic/profiles/&lt;user&gt;/settings.json</code>) →
           <code>TriggerManager()</code> (installs keyboard event filters immediately) →
           <code>AudioManager()</code> →
           <code>VideoManager::open()</code> (opens cameras, or stubs) →
           <code>RecordManager()</code>
         </div>
       </div>
     </div>
     <div class="mosaic-step">
       <div class="mosaic-step-dot">4</div>
       <div class="mosaic-step-body">
         <div class="mosaic-step-title">MainWindow::show()</div>
         <div class="mosaic-step-desc">The app is now interactive.</div>
       </div>
     </div>
   </div>

Video pipeline
--------------

Each configured camera runs its own pair of threads:

.. raw:: html

   <div class="mosaic-diagram mosaic-flow">
     <div class="mosaic-flow-box">
       <div class="mosaic-flow-title">Camera (Pylon)</div>
       <div class="mosaic-flow-sub">or a test-pattern stub if none is connected</div>
     </div>
     <div class="mosaic-flow-arrow">→</div>
     <div class="mosaic-flow-box">
       <div class="mosaic-flow-title">VideoGrabber</div>
       <div class="mosaic-flow-sub">QThread, pushes VideoFrame</div>
     </div>
     <div class="mosaic-flow-arrow">→</div>
     <div class="mosaic-flow-box">
       <div class="mosaic-flow-title">RingBuffer&lt;VideoFrame&gt;</div>
       <div class="mosaic-flow-sub">lock-free SPSC, capacity 128</div>
     </div>
     <div class="mosaic-flow-arrow">→</div>
     <div class="mosaic-flow-box">
       <div class="mosaic-flow-title">VideoEncoder</div>
       <div class="mosaic-flow-sub">QThread</div>
     </div>
     <div class="mosaic-flow-arrow">→</div>
     <div class="mosaic-flow-col">
       <div class="mosaic-flow-box">
         <div class="mosaic-flow-title">video_N.mp4</div>
         <div class="mosaic-flow-sub">FFmpeg / NVENC</div>
       </div>
       <div class="mosaic-flow-box">
         <div class="mosaic-flow-title">timestamps_camN.csv</div>
         <div class="mosaic-flow-sub">FrameTimestampWriter</div>
       </div>
     </div>
   </div>

``VideoManager`` owns all (Grabber, Buffer, Encoder) triples and starts / stops
them together via ``RecordManager``.

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

.. raw:: html

   <div class="mosaic-diagram mosaic-tree">recordings/
   └── 2026-06-04_14-32-05/
       ├── session_meta.json        <span class="mosaic-tree-comment"># cameras, mics, triggers, recorded_by, start UTC</span>
       ├── trigger.csv              <span class="mosaic-tree-comment"># elapsed_ms, wall_clock, source, label, value</span>
       ├── sync_manifest.json       <span class="mosaic-tree-comment"># written after recording stops</span>
       ├── audio/
       │   └── audio_0.wav          <span class="mosaic-tree-comment"># one per microphone</span>
       └── video/
           ├── video_0.mp4          <span class="mosaic-tree-comment"># one per camera</span>
           └── timestamps_cam0.csv  <span class="mosaic-tree-comment"># frame_id, elapsed_ns, wall_ns, hw_timestamp_ns (per camera)</span></div>

Settings persistence
--------------------

Settings are stored as JSON (schema v1) in a per-profile directory:

.. raw:: html

   <div class="mosaic-diagram mosaic-tree">~/.config/CSRU/mosaic/
     profiles.json                  <span class="mosaic-tree-comment"># auth manifest (hashed passwords)</span>
     profiles/
       &lt;username&gt;/
         settings.json              <span class="mosaic-tree-comment"># full AppSettings (cameras, audio, triggers, record)</span>
         mosaic.log</div>

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
   * - ``MOSAIC_HAVE_OPENCV``
     - Calibration in ``CalibrationManager``
   * - ``MOSAIC_HAVE_PARALLEL_PORT``
     - InpOut32 polling in ``ParallelPortTrigger``
