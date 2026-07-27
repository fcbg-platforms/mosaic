Camera settings
===============

.. contents:: On this page
   :local:
   :depth: 2

Where camera settings live
---------------------------

Each configured camera gets its own card in the **Video** settings tab
(``CameraCardW``), with five sub-tabs: **Image**, **Exposure**, **Gain**,
**Advanced**, and **HW Trigger**. Every field is backed by one member of
``CameraParameters`` (``src/core/settings.hpp``) and is persisted per-profile
in that user's ``settings.json`` under ``video.cameras[i]``.

Most fields **live-apply**: editing them pushes the new value straight to the
already-open camera (debounced ~150 ms) without interrupting preview or
recording. A few *structural* changes — resolution, pixel format, frame rate,
and the HW Trigger tab's fields — instead trigger a full close-and-reopen of
every configured camera, since those can't be changed on an already-streaming
GigE device. Both paths go through the same ``settings.json``, so there's no
separate "apply" step to remember.

Image tab
---------

.. list-table::
   :header-rows: 1
   :widths: 25 15 60

   * - Field
     - Default
     - Notes
   * - Width / Height
     - 1920 / 1080 px
     - Sensor ROI size. Structural — triggers a reopen.
   * - Offset X / Y
     - 0 / 0 px
     - ROI top-left corner. Structural.
   * - Reverse X / Y
     - off
     - Horizontal/vertical flip.
   * - Pixel format
     - ``BGR8``
     - One of ``BGR8``, ``RGB8``, ``Mono8``, ``Mono12``, ``BayerRG8``,
       ``BayerBG8``. Structural.
   * - Specify frame rate
     - on
     - If off, the camera free-runs at whatever rate its current
       exposure/ROI/bandwidth allows.
   * - Frame rate
     - 25 fps
     - Matches the ``acA1920-25gc``'s real sustained maximum, not the
       generic "30 fps" a lot of camera UIs default to. Structural — see
       :ref:`good default values <camera-good-defaults>` below for why
       requesting more than a camera can sustain just under-delivers
       silently rather than erroring.

Exposure tab
------------

.. list-table::
   :header-rows: 1
   :widths: 25 15 60

   * - Field
     - Default
     - Notes
   * - Auto mode
     - ``Once``
     - ``Off`` | ``Once`` | ``Continuous`` — see
       :ref:`auto modes explained <camera-auto-modes>` below.
   * - Exposure time
     - 10000 µs
     - Manual value, only used when Auto mode is ``Off``.
   * - Auto range (lower/upper)
     - 100 / 50000 µs
     - Clamps what ``Once``/``Continuous`` is allowed to pick.

Gain tab
--------

.. list-table::
   :header-rows: 1
   :widths: 25 15 60

   * - Field
     - Default
     - Notes
   * - Auto mode
     - ``Once``
     - Same three options as Exposure. **On the ``acA1920-25gc``, manual
       (``Off``) gain does not currently work** — see the callout below.
   * - Gain
     - 0.0 dB
     - Manual value; only meaningful if the camera actually has a working
       manual-gain node (see below).
   * - Auto range (lower/upper)
     - 0.0 / 24.0 dB
     - Clamps what ``Once``/``Continuous`` is allowed to pick.

.. note::
   **Why you may see "Skipping 'Gain': ... No node attached."** — this
   camera generation only exposes the older SFNC 1.x ``GainRaw`` integer
   node, not the modern SFNC 2.0 ``Gain`` (dB) float node Mosaic writes to
   when Auto mode is ``Off``. The write is skipped safely (this is not a
   crash or a real error — see ``VideoGrabber::apply_image_params()`` in
   ``video_grabber.cpp``), but it also means **manual gain is a silent
   no-op on this hardware today**. This is exactly why the default is
   ``Once`` rather than ``Off`` — with manual gain effectively unusable,
   ``Off`` was leaving cameras at whatever gain they happened to power on
   with. If you need a working manual gain control, that would require
   implementing a ``GainRaw`` fallback path (the same pattern already used
   for ``BlackLevel``'s SFNC 1.x fallback) — not done yet.

Advanced tab
------------

.. list-table::
   :header-rows: 1
   :widths: 25 15 60

   * - Field
     - Default
     - Notes
   * - Gamma
     - 1.0
     -
   * - Black level
     - 0.0
     - Falls back to the SFNC 1.x ``BlackLevelRaw`` node on this camera
       generation (the SFNC 2.0 float node isn't present) — this one
       **does** have a working fallback, unlike Gain above.
   * - Saturation / Contrast / Brightness
     - 1.0 / 1.0 / 0.5
     - No GenICam node on the ``acA1920-25gc`` at all (no on-camera ISP for
       these) — saved with the session regardless, but only take visible
       effect on a camera model that exposes the matching node.
   * - Auto target brightness
     - 0.5
     -
   * - Digital shift
     - 0 bits
     - For 12/16-bit → 8-bit conversions.
   * - White balance auto mode
     - ``Once``
     - ``Off`` | ``Once`` | ``Continuous``, same semantics as Exposure/Gain.
   * - Test pattern
     - ``Off``
     - Simulated pattern shown in the monitor when no real camera is
       connected — ``Off`` | ``ColorBars`` | ``Horizontal`` | ``Vertical``.

HW Trigger tab
--------------

Covered in full in :ref:`synchronization`, including what "Action1" actually
does and the live per-camera "Action-command support" readout. Quick
reference:

.. list-table::
   :header-rows: 1
   :widths: 25 15 60

   * - Field
     - Default
     - Notes
   * - Enable hardware trigger
     - on
     - Structural — reopens every configured camera.
   * - Trigger source
     - ``Action1``
     - ``Line1`` | ``Software`` | ``Action1``. ``Action1`` needs no physical
       cable — it's a GigE Vision Action Command broadcast over the
       existing camera network, fired once per frame for the whole
       session.
   * - Trigger delay
     - 0 µs
     - Only meaningful with a physical trigger source (``Line1``); not used
       by ``Action1``.

.. _camera-auto-modes:

Auto modes explained (Exposure, Gain, White Balance)
------------------------------------------------------

All three share the same three-value semantics:

- **Off** — fixed, manual value you set yourself. Never changes on its own.
- **Once** — the camera auto-calibrates a single time (when preview starts),
  then **locks** that value for the rest of the session, including through
  recording.
- **Continuous** — the camera keeps re-adjusting for as long as it's running,
  including *during* recording.

**Continuous** tends to look the best in a live preview, since the camera is
always compensating for whatever's currently in front of it. The tradeoff:
brightness/color can visibly shift mid-recording, which is a real risk if
anything downstream assumes consistent lighting frame-to-frame (facial
expression confidence, luminance-based measures, anything comparing frames
across time). **Once** is the better default for a research recording tool
for exactly that reason — you get a scene-matched image without the
mid-recording drift risk. **Off** gives full manual control but requires you
to already know a good value, and — for Gain specifically on this camera
generation — currently doesn't work at all (see the Gain tab's note above).

.. _camera-good-defaults:

Default values, and where they come from
-------------------------------------------

Every field's default lives in exactly one place: ``CameraParameters``'s own
member-initializers in ``src/core/settings.hpp``. Three code paths all read
from that single source of truth, so there's nowhere else a "default" can
silently diverge:

1. **A brand-new profile** gets seeded with room 11's real 6 camera serials
   (``default_room11_cameras()`` in ``src/core/application.cpp``) — every
   other field is left at ``CameraParameters``'s defaults.
2. **"+ Add camera"** in the Video tab (``VideoSettingsW::add_camera()``)
   constructs a plain default-initialized ``CameraParameters``.
3. **"Discover cameras"** fills in only the serial number and model name it
   found on the network; everything else is, again, left at the defaults.

To change a default for every *future* camera (not ones already saved), edit
the member-initializer in ``CameraParameters`` (``settings.hpp``) and
rebuild — see the Gain/Exposure/White-balance defaults above for a worked
example (``"Off"`` → ``"Once"``, changed 2026-07-27 for exactly the reasons
described in this page).

How to change settings for a camera that already exists
-----------------------------------------------------------

**Existing, already-configured cameras do not pick up a new code default
automatically.** ``CameraParameters::from_json()`` only falls back to the
struct's default when a key is *missing* from ``settings.json`` — for a
camera that's already been saved once, every field is present, "Off"
included, so a code-level default change has no effect on it. Two ways to
actually change an existing camera's settings:

- **Through the UI** (recommended for a one-off/manual change): open the
  relevant tab in that camera's card and change the control directly. Most
  fields live-apply within ~150 ms; HW Trigger and Image-tab structural
  fields reopen the camera.
- **Editing ``settings.json`` directly** (useful for changing several
  cameras at once, or scripting a rollout): find the file at
  ``%LOCALAPPDATA%\CSRU\MOSAIC\settings.json`` (or
  ``profiles\<username>\settings.json`` for a named profile — see
  :doc:`profiles`), and edit the relevant key inside ``video.cameras[i]``,
  e.g.:

  .. code-block:: json

     {
       "video": {
         "cameras": [
           {
             "serial": "24925616",
             "exposure_auto": "Once",
             "gain_auto": "Once",
             "balance_white_auto": "Once"
           }
         ]
       }
     }

  The app must be closed while you edit — it only reads this file at
  startup and rewrites it in full on shutdown, so any change made while it's
  running will be silently overwritten when it next saves.

See also
--------

- :doc:`recording` — :ref:`synchronization` covers the HW Trigger tab and
  ``Action1`` in full detail.
- :doc:`calibration` — intrinsic/extrinsic camera calibration, a separate
  concept from the acquisition settings on this page.
