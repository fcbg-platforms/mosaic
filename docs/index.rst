MOSAIC Documentation
====================

.. toctree::
   :maxdepth: 1
   :hidden:

   quickstart
   architecture
   recording
   calibration
   lsl
   profiles
   api/library_root

.. raw:: html

   <div style="
     background: linear-gradient(135deg, #0d0d28 0%, #111136 100%);
     border: 1px solid #222255;
     border-radius: 10px;
     padding: 28px 32px 20px;
     margin-bottom: 28px;
   ">
     <p style="
       font-size: 1.05rem;
       color: #9999cc;
       margin: 0 0 14px;
       line-height: 1.7;
     ">
       <strong style="color:#c8c8f0;">MOSAIC</strong>
       &nbsp;·&nbsp; <em>Multi-camera Observatory for Social &amp; Activity Interaction Capture</em>
     </p>
     <p style="font-size: 0.92rem; color: #6666aa; margin: 0;">
       Qt 6 · C++23 · Basler Pylon · OpenCV · Lab Streaming Layer
     </p>
   </div>

.. grid:: 2
   :gutter: 3

   .. grid-item-card:: 🚀  Quick start
      :link: quickstart
      :link-type: doc
      :class-card: sd-border-0

      Build MOSAIC, deploy to a recording station, and capture your first
      multi-camera session in under ten minutes.

   .. grid-item-card:: 🏗  Architecture
      :link: architecture
      :link-type: doc
      :class-card: sd-border-0

      How the subsystems fit together: video pipeline, audio, triggers,
      calibration, and the profile system.

   .. grid-item-card:: 🎬  Recording workflow
      :link: recording
      :link-type: doc
      :class-card: sd-border-0

      Session folders, file formats, per-frame timestamp CSV, and the
      ``session_meta.json`` sidecar.

   .. grid-item-card:: 📐  Camera calibration
      :link: calibration
      :link-type: doc
      :class-card: sd-border-0

      Checkerboard intrinsic calibration with OpenCV; results stored
      per-camera and per-profile.

   .. grid-item-card:: ⚡  LSL integration
      :link: lsl
      :link-type: doc
      :class-card: sd-border-0

      Sync MOSAIC with EEG, eye-trackers, and other physiological
      recorders via Lab Streaming Layer.

   .. grid-item-card:: 👤  Profiles
      :link: profiles
      :link-type: doc
      :class-card: sd-border-0

      Per-research-group profiles: isolated configs, PBKDF2 passwords,
      and the in-app profile switcher.

   .. grid-item-card:: 📖  API reference
      :link: api/library_root
      :link-type: doc
      :class-card: sd-border-0

      Full C++ class and function documentation generated from source
      via Doxygen + Breathe.

----

Key capabilities
----------------

.. list-table::
   :widths: 25 75
   :header-rows: 0
   :class: colwidths-auto

   * - **Multi-camera**
     - Basler Pylon SDK with per-camera exposure, gain, ROI, pixel format, and
       hardware trigger control; test-pattern stub for development without hardware.

   * - **Synchronised audio**
     - Per-microphone WAV recording via ``QAudioSource`` with real-time RMS
       waveform display.

   * - **Precise timestamps**
     - Every video frame carries a monotonic ``elapsed_ns`` and wall-clock
       ``wall_ns``; written to ``timestamps_camN.csv`` alongside the MP4.

   * - **Trigger logging**
     - Keyboard bindings, LSL inlets, parallel-port TTL edges — all written
       to a CSV with nanosecond resolution.

   * - **LSL outlet**
     - One sample per frame published so downstream recorders (EEG, eye-tracker)
       can align to MOSAIC's timeline with sample-level accuracy.

   * - **Camera calibration**
     - OpenCV checkerboard intrinsic calibration; results stored per camera in
       the group's ``settings.json``.

   * - **Research profiles**
     - Isolated settings, recording directories, and calibration per group;
       PBKDF2-SHA256 login.
