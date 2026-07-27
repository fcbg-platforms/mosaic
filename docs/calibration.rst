Camera calibration
==================

.. contents:: On this page
   :local:
   :depth: 2

Overview
--------

MOSAIC uses OpenCV's checkerboard calibration to compute per-camera **intrinsic
parameters** (focal length, principal point, distortion coefficients), and a
separate ChArUco-based procedure to compute each camera's **extrinsic
pose** (position/orientation in one shared room coordinate frame). Results
are stored in each camera's :cpp:struct:`mosaic::CalibrationData` field
inside the group's ``settings.json`` and are automatically embedded in
``session_meta.json`` so downstream pose-estimation and gaze-fusion tools can
use them without manual bookkeeping.

Requirements: build with ``-DMOSAIC_ENABLE_OPENCV=ON``.  The **Calibrate** tab
in the settings sidebar shows a helpful "not available" message otherwise.
It has two inner tabs: **Intrinsics** (single-camera, this page) and
**Room (Extrinsics)** (multi-camera, simultaneous capture — see below).

Checkerboard setup
------------------

Print or display a checkerboard pattern and measure the physical size of one
square in millimetres.  The recommended starting point is:

- **9 × 6 inner corners** (a 10 × 7 square board)
- **25 mm squares** (A4 sheet printed at 100 %)

Ensure the board is:

- Flat (a rigid foam-core mount is better than paper alone).
- Large enough that it fills at least 1/3 of the frame.
- Printed without any scaling — measure a square with calipers to confirm.

Intrinsic calibration procedure
--------------------------------

1. Open the **Calibrate** tab in the settings sidebar, **Intrinsics** page.
2. Set **Cols**, **Rows**, and **Square size** to match your board.
3. Select the camera to calibrate from the **Camera** dropdown.
4. Move the board to different positions, orientations, and distances in front
   of the camera.  Each time a good view appears, click **Capture frame** (or
   call :cpp:func:`mosaic::CalibrationManager::feed_frame` from code).
   The preview updates to show detected corners.
5. Collect at least **10 accepted views** — aim for 20–30 at varied angles.
6. Click **▶ Calibrate**.  The result appears within a few seconds:

   - **RMS error < 0.5 px** — excellent.
   - **0.5 – 1.0 px** — good; acceptable for most pose-estimation tasks.
   - **> 2.0 px** — poor; recapture with better coverage.

7. Click **Save calibration to settings**.  The result is written to the
   current group's ``settings.json`` and will appear in all future
   ``session_meta.json`` files.

.. warning::

   Calibration is per-camera *mount*.  If you move a camera or change its lens,
   recalibrate.  The **serial number** stored in ``CameraParameters`` makes it
   possible to track which physical camera a calibration belongs to.

Room (extrinsic) calibration procedure
---------------------------------------

Every camera used here must already have a valid **intrinsic** calibration
(above) — the **Room (Extrinsics)** page will not solve a camera that hasn't
been calibrated.

1. Open the **Calibrate** tab, **Room (Extrinsics)** page.
2. Set the ChArUco board's **Cols**, **Rows**, **Square size**, and
   **Marker size** (a ChArUco board tolerates partial views, unlike the plain
   checkerboard above — useful since wide-FOV cameras around a room see the
   board from very different angles).
3. Hold the board somewhere visible to at least two cameras and click
   **▶ Capture shot**. Repeat, moving the board through overlapping pairs of
   camera fields of view, until every camera has been in at least one shot
   shared with an already-resolved camera — 8+ shots is a reasonable start.
4. Pick a **reference camera** (its pose becomes the room's origin, identity
   transform) and click **▶ Solve**. The per-camera result table shows which
   cameras resolved and their reprojection RMS (aim for < 2 px, same guidance
   as intrinsics above); an unresolved camera means no shared-shot chain back
   to the reference camera was found — capture another shot linking it in.
5. To define the room's shared reference plane (used by gaze-fusion's
   target-point intersection): lay the board flat on the target surface,
   capture one more shot, then click **Use last shot as plane**.
6. Click **Save to settings** to persist both the per-camera extrinsics and
   the plane into the group's ``settings.json``.

What is stored
--------------

:cpp:struct:`mosaic::CalibrationData` stores:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Field
     - Description
   * - ``calibrated``
     - ``true`` after a successful calibration run.
   * - ``rmsError``
     - Mean reprojection error in pixels (lower is better).
   * - ``cameraMatrix[9]``
     - 3 × 3 camera matrix, row-major: ``[fx 0 cx; 0 fy cy; 0 0 1]``.
   * - ``distCoeffs[5]``
     - Distortion coefficients ``[k1, k2, p1, p2, k3]``.
   * - ``extrinsicRt[16]``
     - 4 × 4 homogeneous RT, row-major, relative to the chosen reference
       camera (identity for the reference camera itself). Populated by the
       **Room (Extrinsics)** ChArUco procedure above; stays at its default
       identity for any camera that hasn't been resolved yet.
   * - ``extrinsicCalibrated``
     - ``true`` once this camera's extrinsics were successfully resolved by
       the Room (Extrinsics) procedure — distinct from ``calibrated``, which
       only ever means "intrinsics done".

Room (extrinsic) calibration solves every camera's position and orientation
relative to one shared room origin — needed to combine per-camera 3D
signals (e.g. gaze rays, see :doc:`math/gaze_fusion`) across cameras. See
:doc:`user_guide`'s calibration workflow for a narrative walkthrough of the
same steps, and :doc:`math/room_calibration` for the underlying pose-graph
math.

Using calibration data in Python
---------------------------------

.. code-block:: python

   import json, numpy as np, pathlib

   meta   = json.loads(pathlib.Path("session_meta.json").read_text())
   cam    = meta["cameras"][0]
   cal    = cam["calibration"]

   K = np.array(cal["camera_matrix"]).reshape(3, 3)
   D = np.array(cal["dist_coeffs"])

   print("Focal length: fx={:.1f}  fy={:.1f}".format(K[0,0], K[1,1]))
   print("Principal pt: cx={:.1f}  cy={:.1f}".format(K[0,2], K[1,2]))
   # K and D can be passed directly to cv2.undistort() or DeepLabCut

``session_meta.json`` also carries a session-wide ``room`` section (the
reference plane defined in step 5 above): ``plane_point``/``plane_normal``
(3-vectors in room coordinates) and ``plane_defined``. The Analysis tab's
**Multi-Camera Gaze Fusion** plugin reads both the per-camera ``extrinsic_rt``
values and this ``room`` section to triangulate a 3D gaze ray per moment and,
where the plane is defined, intersect it with the target surface — see
``analysis/run_gaze_fusion.py`` and the resulting ``gaze_fusion.json``.
