Recording workflow
==================

.. contents:: On this page
   :local:
   :depth: 2

Starting a session
------------------

A recording session is controlled by :cpp:class:`mosaic::RecordManager`.
The simplest way to start one is via the **● Record** button in the QML
monitor view or ``Ctrl+R`` — both call
:cpp:func:`mosaic::MonitorBridge::startRecording`.

Programmatically:

.. code-block:: cpp

   // RecordManager is created by Application and accessible via:
   RecordManager* rm = app.record_manager();

   if (rm->start()) {
       qDebug() << "Recording to:" << rm->current_session_path();
   }

   // ... later ...
   rm->stop();

What happens during ``start()``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **Session folder** is created:
   ``<record.directory>/<timestamp>/`` (e.g. ``recordings/2026-06-04_14-32-05/``).
2. **``session_meta.json``** is written immediately (see :ref:`session metadata`).
3. **Trigger CSV** is opened: ``trigger.csv``.
4. **Audio recorders** are started: one ``WAV`` file per configured microphone.
5. **Video grabbers and encoders** are started: one ``MP4`` + one ``timestamps_camN.csv`` per camera.
6. The **elapsed timer** fires every 100 ms, updating the HH:MM:SS display.

.. _session metadata:

``session_meta.json``
---------------------

Written at the start of every session, regardless of whether recording
succeeds later.  Fields:

.. code-block:: json

   {
     "schema":                   "mosaic-session-v1",
     "mosaic_version":           "0.1.0",
     "recorded_by":              "cognitive_lab",
     "session_start_utc":        "2026-06-04T14:32:05.123Z",
     "session_start_elapsed_ns": 12345678,
     "session_folder":           "/home/user/recordings/2026-06-04_14-32-05",
     "cameras": [
       {
         "index": 0,
         "serial": "12345678",
         "name": "Camera 1",
         "width": 1920, "height": 1080, "fps": 30.0,
         "pixel_format": "BGR8",
         "codec": "h264_nvenc",
         "calibration": { "calibrated": true, "rms_error": 0.312 }
       }
     ],
     "microphones": [
       { "index": 0, "name": "RODE NT-USB", "sample_rate": 44100, "channels": 2 }
     ],
     "trigger_sources": {
       "keyboard":    [ { "name": "Event A", "key_seq": "F1" } ],
       "lsl_inlets":  [ { "name": "EEG Markers", "stream_name": "BrainAmp" } ],
       "parallel_ports": [],
       "lsl_outlet_name": "MOSAIC"
     }
   }

Timestamp files
---------------

Each camera produces a ``timestamps_camN.csv`` alongside its ``video_N.mp4``:

.. code-block:: text

   frame_id,elapsed_ns,wall_ns,hw_timestamp_ns
   1,1234567,1717506725000000000,88123456000
   2,1267890,1717506725033333333,88156789000
   ...

- **``frame_id``** — monotonic counter starting at 1, resets each session.
- **``elapsed_ns``** — nanoseconds from ``steady_clock`` since application start.
  Use this for aligning video frames to trigger events (both use the same clock).
- **``wall_ns``** — nanoseconds since Unix epoch (``system_clock``).
  Use this for absolute alignment to external recordings.
- **``hw_timestamp_ns``** — the camera's own hardware timestamp (GigE Vision
  ``GevTimestamp`` chunk, converted from device ticks to nanoseconds), if the
  connected camera and SDK support chunk data. ``0`` if unavailable. This is
  independent of host-side scheduling/network jitter, but is **not**
  comparable across cameras unless they share a hardware trigger/genlock
  source (see :ref:`synchronization` — Mosaic does not do this today).

.. note::

   ``elapsed_ns`` and ``wall_ns`` are both stamped **at grab time** inside
   the ``VideoGrabber`` thread — before the frame is handed to the encoder.
   This gives the most accurate timestamp possible for each frame.

.. _synchronization:

Synchronization
----------------

Mosaic does **not** use a hardware trigger or genlock to force all cameras to
expose simultaneously. Each camera free-runs at its own configured frame
rate, and synchronization across cameras is done **post-hoc**, after
recording, by :cpp:class:`mosaic::SyncManifest`:

1. It reads every camera's ``timestamps_camN.csv``.
2. It finds the overlapping time window (on the shared ``elapsed_ns`` clock)
   across all cameras.
3. It builds a uniform "master tick" timeline (default 25 fps) and, for each
   tick, finds the nearest real frame per camera plus its timing error
   (``delta_ms``).
4. The result (``sync_manifest.json``) is what :cpp:class:`mosaic::SessionPlayerW`
   uses to play all cameras back in sync.

This means synchronization quality is a **playback-time property**, not an
acquisition-time guarantee — two cameras' frame N are not guaranteed to be
the same physical instant, only mapped to the nearest common tick after the
fact.

Why cameras end up with different frame counts
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A few causes, all independent of each other:

- **Grabbers start sequentially, not simultaneously** — ``VideoManager::start()``
  prepares every camera's encoder first, then starts all grab threads
  back-to-back, so the very first frame time differs slightly per camera.
- **A camera can fail to open** (duplicate/ambiguous serial number, or a
  hardware/Pylon error) — ``VideoManager::open()`` simply skips it, leaving
  that camera with zero frames for the whole session while the others run
  fine.
- **Per-camera ring-buffer overflow or GigE packet loss** — each camera has
  its own 128-slot ring buffer between the grab and encode threads; if the
  encoder falls behind (e.g. due to GigE packet loss / incomplete frames)
  that camera alone drops frames.
- **The requested frame rate may not be achievable.** ``AcquisitionFrameRate``
  is a *request* — Pylon accepts values the camera cannot actually sustain
  at the current exposure time / ROI / GigE bandwidth without raising an
  error, and simply under-delivers. Mosaic now logs a warning
  (``VideoGrabber::open()``) comparing the requested rate against
  ``ResultingFrameRate``/``ResultingFrameRateAbs`` (the camera's own estimate
  of its actual achievable rate) whenever they diverge by more than 10%, so
  this shows up in ``mosaic.log`` instead of silently producing fewer frames
  than expected.

Why the per-camera frame rate is uniform, and only *transmission* is staggered
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

All cameras are configured to the *same* requested ``AcquisitionFrameRate``
(each camera's own ``CameraParameters::fps``) — this is required for
:cpp:class:`mosaic::SyncManifest`'s uniform master-tick model to produce a
meaningful alignment; per-camera frame *rates* are not staggered.

What **is** staggered is each camera's GigE *packet transmission* timing
(``GevSCFTD``, set to ``camera_index × 5 ms`` in ``VideoGrabber::open()``) —
this only delays when a captured frame's packets go out on the wire, so that
all 6 cameras don't burst onto the same network card simultaneously and
saturate it. It has no effect on exposure timing or capture rate, and is
unrelated to synchronization; it exists purely to reduce GigE packet loss.

If frame-accurate simultaneous exposure across cameras is required (rather
than post-hoc nearest-tick alignment), that needs a real hardware
trigger/genlock signal wired to every camera's trigger input — the camera's
own ``TriggerMode``/``TriggerSource``/``TriggerDelay`` nodes are wired (see
the *HW Trigger* tab in the Video settings), but nothing currently drives a
shared trigger signal to all cameras simultaneously; this is future work
contingent on the lab's actual trigger wiring.

Trigger CSV
-----------

Every trigger event (keyboard, LSL marker, parallel port edge) is appended to
``trigger.csv``:

.. code-block:: text

   elapsed_ms,wall_clock,source,label,value
   1523,14:32:06.645,keyboard,Event A,0
   2100,14:32:07.222,lsl,Stimulus/S1,1

Aligning streams in Python
--------------------------

A minimal alignment example:

.. code-block:: python

   import pandas as pd
   import json, pathlib

   session = pathlib.Path("recordings/2026-06-04_14-32-05")
   meta    = json.loads((session / "session_meta.json").read_text())

   # Video frame timestamps
   frames  = pd.read_csv(session / "timestamps_cam0.csv")

   # Trigger events
   triggers = pd.read_csv(session / "trigger.csv")

   # Align: find the nearest frame for each trigger
   def nearest_frame(elapsed_ns):
       idx = (frames["elapsed_ns"] - elapsed_ns).abs().idxmin()
       return frames.loc[idx, "frame_id"]

   triggers["frame_id"] = triggers["elapsed_ms"].apply(
       lambda ms: nearest_frame(ms * 1_000_000)
   )
   print(triggers[["label", "frame_id"]].head())
