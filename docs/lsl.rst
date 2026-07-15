LSL integration
===============

.. contents:: On this page
   :local:
   :depth: 2

Overview
--------

`Lab Streaming Layer (LSL) <https://labstreaminglayer.org/>`_ is the de-facto
synchronisation protocol in neuroscience and behavioural research.  MOSAIC
integrates LSL in both directions:

- **Outlet** — one sample per video frame, letting any LSL-aware recorder
  (EEG amplifiers, eye-trackers, physiological amplifiers) align its data to
  MOSAIC's camera timeline.
- **Inlets** — one background thread per configured inlet, converting received
  markers into :cpp:struct:`mosaic::TriggerEvent` objects that are logged to
  ``trigger.csv`` alongside keyboard and parallel-port events.

Requirements: build with ``-DMOSAIC_ENABLE_LSL=ON`` and install liblsl via
vcpkg (``vcpkg install mosaic[lsl]``).

Frame-marker outlet
-------------------

When enabled, MOSAIC opens a stream named ``<outletName>_Video`` (default:
``MOSAIC_Video``) of type ``VideoFrames``:

.. list-table::
   :header-rows: 1
   :widths: 15 20 65

   * - Channel
     - Label
     - Description
   * - 0
     - ``camera_index``
     - Index of the camera (0, 1, 2 …).
   * - 1
     - ``frame_id``
     - Monotonic frame counter, starting at 1 for each session.
   * - 2
     - ``elapsed_ns``
     - Nanoseconds since application start (``steady_clock``).

A second stream, ``<outletName>_Events``, publishes string markers:

.. list-table::
   :header-rows: 1

   * - Marker
     - When
   * - ``SESSION_START``
     - ``RecordManager::start()`` succeeds.
   * - ``SESSION_STOP``
     - ``RecordManager::stop()`` is called.
   * - ``<trigger label>``
     - Any keyboard or parallel-port trigger fires.

Configuration
~~~~~~~~~~~~~

In the **Triggers** settings tab, under *LSL outlet*:

- **Enable LSL outlet** — toggles the outlet.
- **Outlet name** — prefix for both stream names.
- **Nominal rate** — declared sample rate (used by resolvers; actual rate
  equals the configured camera FPS).

Receiving MOSAIC markers in Python (MNE)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: python

   import pylsl, mne

   # Resolve the MOSAIC event stream
   streams = pylsl.resolve_byprop("name", "MOSAIC_Events", timeout=5)
   inlet   = pylsl.StreamInlet(streams[0])

   samples, timestamps = inlet.pull_chunk(timeout=1.0)
   for marker, ts in zip(samples, timestamps):
       print(f"{ts:.3f}  {marker[0]}")

Aligning with an EEG recording
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: python

   # After recording: both the EEG file and MOSAIC timestamps_cam0.csv
   # share the LSL global clock.
   import pandas as pd, pylsl

   frames    = pd.read_csv("timestamps_cam0.csv")
   # The EEG software's event table has LSL timestamps in seconds.
   # elapsed_ns in the CSV uses steady_clock — convert via the SESSION_START
   # marker's LSL timestamp:

   # 1. Find SESSION_START timestamp from the EEG marker channel.
   session_start_lsl = ...   # from your EEG software's event table

   # 2. The first frame_id=1 sample on MOSAIC_Video has elapsed_ns ≈ 0
   #    (very small, typically < 5 ms after session start).
   #    Use the LSL timestamp of that sample as the common reference.
   video_start_lsl   = ...   # first MOSAIC_Video sample timestamp

   frames["lsl_time"] = video_start_lsl + frames["elapsed_ns"] / 1e9
   # Now frames["lsl_time"] is on the same axis as your EEG events.

Configuring inlets
------------------

Add one inlet per external LSL stream you want to record as triggers.
Each inlet resolves the stream by name in a background thread with a 0.5 s
non-blocking timeout (will reconnect automatically if the stream appears later).

In the **Triggers** settings tab, under *LSL inlets*:

1. Click **+ Add LSL inlet**.
2. Set **Stream name** to the exact ``name`` field of the stream you want
   (e.g. ``BrainAmp``, ``TobiiGlasses3``).
3. Set **Stream type** (optional; used as a secondary filter).
4. Enable the inlet.

Received string samples are written to ``trigger.csv`` with ``source = lsl``
and the sample string as the ``label``.

.. note::

   Numeric LSL samples are not yet supported.  If your stream sends floats or
   ints, convert them to string markers in a thin Python bridge script, or open
   a GitHub issue requesting native numeric inlet support.
