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
4. **Audio recorders** are started: one ``WAV`` file per configured microphone, under
   ``<session>/audio/``.
5. **Video grabbers and encoders** are started: one ``MP4`` + one ``timestamps_camN.csv`` per
   camera, under ``<session>/video/``.
6. The **elapsed timer** fires every 100 ms, updating the HH:MM:SS display.

Session folder layout:

.. code-block:: text

   recordings/
   └── <username>/                        # per-profile — see :doc:`profiles`
       └── 2026-06-04_14-32-05/
           ├── session_meta.json
           ├── trigger.csv
           ├── trigger_frame_map.json         # written by the EEG/Trigger↔Frame Sync plugin, if run
           ├── sync_manifest.json             # written after recording stops
           ├── gaze_fusion.json                # written by run_gaze_fusion.py, if run
           ├── skeleton3d.json                 # written by run_pose3d.py, if run
           ├── audio/
           │   └── audio_0.wav
           ├── video/
           │   ├── video_0.mp4
           │   └── timestamps_cam0.csv
           ├── pose/
           │   └── video_0.<model-slug>.pose.json      # written by run_pose.py — filename is
           │                                            # model-namespaced, so running two
           │                                            # different models keeps both results
           ├── depth/
           │   └── video_0.<model-slug>.mp4             # a depth-task Pose model's colorized
           │                                            # output — no keypoints/JSON, see the
           │                                            # note below
           ├── expression/
           │   └── video_0.expression.json    # written by run_expression.py, if run
           ├── rppg/
           │   └── video_0.<backend>.rppg.json  # written by run_rppg.py, if run — EXPERIMENTAL
           └── anonymized/
               └── video_0.mp4                 # written by run_face_mask.py, if run — never
                                                # touches the original video/ files

Media is split into ``audio/`` and ``video/`` subfolders so a session directory listing isn't
dominated by per-camera files; ``pose/``, ``depth/``, ``expression/``, and ``rppg/`` each hold
their own plugin's per-camera output, kept out of ``video/`` rather than sitting alongside the
source ``.mp4`` files; ``anonymized/`` holds Face Masking's output videos; everything session-level
(metadata, trigger log, sync manifest, cross-camera fusion results, annotations) stays at the
session root. Recordings are also split **per profile** — see :doc:`profiles`'s recording-access-
control section for how a non-admin profile's sessions stay isolated from every other profile's.

.. note::

   A Pose-plugin **depth model** (e.g. ``yolo26n-depth``) writes only a
   colorized depth video to ``depth/`` — it produces no keypoints and
   therefore no ``.pose.json`` at all, since a depth-task model's output
   head has no boxes/keypoints to extract in the first place. Run Pose
   twice on the same session — once with a regular pose model, once with a
   depth model — to get both a ``pose/`` result and a ``depth/`` result;
   they coexist in separate subfolders.

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
       "keyboard":       [ { "name": "Event A", "key_seq": "F1" } ],
       "parallel_ports": [ { "port_address": "0xAEFC" } ]
     }
   }

Timestamp files
---------------

Each camera produces a ``timestamps_camN.csv`` alongside its ``video_N.mp4``, both under
``<session>/video/``:

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
  directly comparable across cameras unless they were driven by a shared
  trigger source during acquisition (see :ref:`synchronization` for when
  that's the case).

.. note::

   ``elapsed_ns`` and ``wall_ns`` are both stamped **at grab time** inside
   the ``VideoGrabber`` thread — before the frame is handed to the encoder.
   This gives the most accurate timestamp possible for each frame.

.. _synchronization:

Synchronization
----------------

Mosaic has two layers of synchronization, one acquisition-time and one
playback-time. Which one actually determines simultaneity for a given
recording depends on whether each camera's hardware trigger is enabled.

Acquisition-time: GigE Vision Action Command triggering
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When a camera's *HW Trigger* tab (Video settings) has ``hwTriggerEnabled``
on and ``hwTriggerSource`` set to ``"Action1"``, that camera is configured
with ``TriggerSelector=FrameStart`` / ``TriggerMode=On`` / ``TriggerSource=
Action1`` and waits for a GigE Vision Action Command broadcast before
exposing each frame — it does not free-run. ``VideoManager`` arms every
such camera (``start_grabbing()``) first, then a dedicated background
thread (``ActionCommandTicker``) broadcasts one Action Command per frame,
continuously, at a shared period derived from the *slowest* participating
camera's own measured achievable frame rate — so no camera is ever driven
faster than it can sustain. Every Action1-armed camera therefore exposes
each frame in response to the same broadcast, giving real acquisition-time
simultaneity rather than an after-the-fact approximation.

This is **per-camera, not all-or-nothing**: support is probed live each
time a camera opens (some GigE firmware/node-map combinations don't expose
the required ``ActionSelector``/``ActionDeviceKey``/``ActionGroupKey``/
``ActionGroupMask`` nodes), and a camera that doesn't support it — or
simply has ``hwTriggerSource`` set to something else (``Line1``,
``Software``, plain free-run) — falls back to free-running independently,
exactly as described below, with zero effect on the other cameras. The HW
Trigger tab shows a live "Action-command support: SUPPORTED / NOT
supported" readout per camera so this never needs guessing.

Practically: as of the room 11 camera fleet's current configuration, every
camera defaults to ``hwTriggerEnabled=true`` / ``hwTriggerSource="Action1"``,
so a normal recording session already gets real hardware-triggered
simultaneity, not just the post-hoc alignment below — confirmed on real
hardware, with all 6 cameras firing off the same continuous per-frame
broadcast and producing matching frame counts. Real GigE packet loss on a
specific camera's link can still cause that one camera to miss some
broadcasts (indistinguishable in effect from any other dropped-frame cause
below), which is why the post-hoc layer remains in place regardless.

Playback-time: post-hoc alignment via SyncManifest
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Whether or not hardware triggering was active, Mosaic always additionally
builds a **post-hoc** alignment after recording, via
:cpp:class:`mosaic::SyncManifest`:

1. It reads every camera's ``timestamps_camN.csv``.
2. It finds the overlapping time window (on the shared ``elapsed_ns`` clock)
   across all cameras.
3. It builds a uniform "master tick" timeline (default 25 fps) and, for each
   tick, finds the nearest real frame per camera plus its timing error
   (``delta_ms``).
4. The result (``sync_manifest.json``) is what :cpp:class:`mosaic::SessionPlayerW`
   uses to play all cameras back in sync.

This is what every analysis plugin that fuses across cameras (gaze fusion,
3D pose reconstruction) actually reads — they consume the master-tick
timeline, not raw per-camera frame indices, so they benefit from tighter
alignment when hardware triggering was active without needing to know
whether it was. When hardware triggering was **not** active for a session,
this layer is the *only* synchronization, and its quality is a
playback-time property, not an acquisition-time guarantee — two cameras'
frame N are not guaranteed to be the same physical instant, only mapped to
the nearest common tick after the fact.

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

Frame-accurate simultaneous exposure across cameras is available today via
the GigE Vision Action Command triggering described above (``hwTriggerSource
= "Action1"``) — no physical trigger cable/genlock wiring is required, since
Action Commands travel over the existing camera network. A physical
hardware-trigger-cable path (``Line1``/``Software`` sources, the camera's
own ``TriggerMode``/``TriggerSource``/``TriggerDelay`` nodes) also exists in
the *HW Trigger* tab for labs that do have a wired trigger signal, but it is
not required for acquisition-time sync on this rig. True PTP-synchronized
(sub-millisecond) scheduled triggering was investigated and found
unsupported by this camera generation's firmware (no ``GevIEEE1588`` node)
— Action Command triggering is the ceiling on this hardware, not an interim
step toward something tighter.

Occasional missed frames are normal — telling jitter from a real problem
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Even with Action Command triggering active and every camera healthy, a
long recording will very occasionally miss an isolated trigger — one
camera skips a single frame (or a handful in a row) and picks the very
next one up cleanly. On a real 30-minute, 5-camera room-11 session this
showed up as 0–11 gaps of a few hundred milliseconds per camera out of
23,000+ frames each (well under 0.05% of intervals), with every camera's
total frame count within 0.3% of the others. **This is expected background
jitter on any GigE Vision network — a brief switch/NIC hiccup that
self-corrects on the next trigger — not a bug, and not something to chase.**

A *real* problem looks nothing like that, and is easy to tell apart in
``mosaic.log``:

- ``[VideoManager] Camera N: <ticks> action-command ticks fired so far but
  only <captured> frames captured (<missing> missing) — this camera is
  likely missing trigger broadcasts, not just corrupted frames.`` —
  recurring every ~5s for the same camera, not a one-off.
- ``[Camera N] <count> incomplete frame(s) in last 5 s (GigE packet loss —
  check NIC jumbo frames and switch bandwidth)`` — also recurring every ~5s.

Either warning showing up **repeatedly, for the same camera, session after
session** points to a real physical fault on that camera's link (cable,
connector, or NIC port) — confirmed on room 11's own hardware: one camera
on a marginal link lost 15–85% of its frames with these exact warnings
firing continuously, while the other 5 cameras on healthy links stayed
within a fraction of a percent of each other. The fix in that case is
physical (reseat, then swap the cable/connector if reseating doesn't
hold), not a settings or code change. To check a session after the fact,
compare each camera's ``timestamps_camN.csv`` line count against the
others — healthy cameras land within roughly 1% of each other; a camera
sitting far below the rest is the one to investigate.

Trigger CSV
-----------

Every trigger event (keyboard, serial byte, parallel port edge — e.g. an EEG
amplifier's trigger-out cable) is appended to ``trigger.csv``:

.. code-block:: text

   elapsed_ms,elapsed_ns,wall_clock,source,label,value
   1523.004,1523004112000,14:32:06.645,keyboard,Event A,0
   4910.331,4910331889000,14:32:09.032,parallel_port,D3_RISE,1

- **``elapsed_ns``** — the raw, unmodified value from the same ``elapsed_ns()``
  origin as ``timestamps_camN.csv``'s own ``elapsed_ns`` column. **Use this
  column** for any cross-file alignment — it needs no reconstruction.
- **``elapsed_ms``** — recording-relative (zeroed when the recording started,
  not when the app launched). Convenient for skimming a session by eye, but
  **not** safe to compare directly against ``timestamps_camN.csv`` — it uses
  a different zero-point.

.. note::

   Sessions recorded before Mosaic added the ``elapsed_ns`` column only have
   the older 5-column schema (no reliable cross-file alignment is possible
   for those — the original zero-point offset was never persisted anywhere).

Aligning streams in Python
--------------------------

The Analysis tab's **"EEG/Trigger ↔ Frame Sync"** plugin does this
automatically — it resolves every trigger event to its nearest frame in
every camera, shows the result in a click-to-seek table, and exports it as
CSV/JSON (:cpp:class:`mosaic::TriggerFrameMap`). The manual equivalent, for
scripting against a session directly:

.. code-block:: python

   import pandas as pd
   import json, pathlib

   session = pathlib.Path("recordings/2026-06-04_14-32-05")
   meta    = json.loads((session / "session_meta.json").read_text())

   # Video frame timestamps
   frames  = pd.read_csv(session / "video" / "timestamps_cam0.csv")

   # Trigger events
   triggers = pd.read_csv(session / "trigger.csv")

   # Align: find the nearest frame for each trigger, using the raw
   # elapsed_ns column directly — no offset reconstruction needed, since
   # both files share the same elapsed_ns() clock origin.
   def nearest_frame(elapsed_ns):
       idx = (frames["elapsed_ns"] - elapsed_ns).abs().idxmin()
       return frames.loc[idx, "frame_id"]

   triggers["frame_id"] = triggers["elapsed_ns"].apply(nearest_frame)
   print(triggers[["label", "frame_id"]].head())
