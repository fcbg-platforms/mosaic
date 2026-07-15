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

   frame_id,elapsed_ns,wall_ns
   1,1234567,1717506725000000000
   2,1267890,1717506725033333333
   ...

- **``frame_id``** — monotonic counter starting at 1, resets each session.
- **``elapsed_ns``** — nanoseconds from ``steady_clock`` since application start.
  Use this for aligning video frames to trigger events (both use the same clock).
- **``wall_ns``** — nanoseconds since Unix epoch (``system_clock``).
  Use this for absolute alignment to external recordings.

.. note::

   ``elapsed_ns`` and ``wall_ns`` are both stamped **at grab time** inside
   the ``VideoGrabber`` thread — before the frame is handed to the encoder.
   This gives the most accurate timestamp possible for each frame.

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
