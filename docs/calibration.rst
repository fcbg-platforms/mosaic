Camera calibration
==================

.. contents:: On this page
   :local:
   :depth: 2

Overview
--------

MOSAIC uses OpenCV's checkerboard calibration to compute per-camera **intrinsic
parameters** (focal length, principal point, distortion coefficients), and a
separate procedure, with a ChArUco board or a plain chessboard with one
marked corner square, to compute each camera's **extrinsic pose**
(position/orientation in one shared room coordinate frame). Results
are stored in each camera's :cpp:struct:`mosaic::CalibrationData` field
inside the group's ``settings.json`` and are automatically embedded in
``session_meta.json`` so downstream pose-estimation and gaze-fusion tools can
use them without manual bookkeeping.

Requirements: build with ``-DMOSAIC_ENABLE_OPENCV=ON``.  The **Calibrate** tab
in the settings sidebar shows a helpful "not available" message otherwise.
It has two inner tabs: **Intrinsics** (single-camera, this page) and
**Room (Extrinsics)** (multi-camera, simultaneous capture; see below).

Checkerboard setup
------------------

Print or display a checkerboard pattern and measure the physical size of one
square in millimetres. MOSAIC asks for the number of **squares** along each
side (black and white together), as one counts them on the board. The
recommended starting point is:

- **10 × 7 squares** (9 × 6 inner corners)
- **22 mm squares** (A4 sheet printed at 100 %)

A large board (for example a 9 × 9 chessboard with 80 to 100 mm squares)
works as well and is easier to see from across the room. Measure across
several squares and divide, rather than one square: every distance MOSAIC
measures later is in proportion to this number.

Ensure the board is:

- Flat (a rigid foam-core mount is better than paper alone).
- Large enough that it fills at least 1/3 of the frame.
- Printed without any scaling; measure a square with calipers to confirm.

Intrinsic calibration procedure
--------------------------------

1. Open the **Calibrate** tab in the settings sidebar, **Intrinsics** page.
2. Set **Squares across**, **down** and **Square** to match your board.
3. Select the camera to calibrate from the **Camera** dropdown. Each
   captured view stores its corners in this board's size and belongs to this
   camera, so the board fields and the camera are locked once a view is
   captured; click **Clear views** to start over with another board or
   camera.
4. Hold the whole board in the camera's view. About once a second a full
   resolution frame is checked; when the board is found and has moved since
   the last kept view, the view is kept. The line under the view count says
   what happened to each frame (not found, not moved, a different image
   size). Move the board between views: near and far, into all four corners
   and along the edges of the picture, tilted about 30 degrees left, right,
   up and down. **Frame covered** is the share of the picture where corners
   have been seen; the lens distortion is only measured there, so aim for
   most of it.
5. Collect at least **10 accepted views**; aim for 15–25.
6. Click **▶ Calibrate**.  The result appears within a few seconds. Views
   that fit far worse than the rest (over 1 px and over 3 times the median,
   a blurred frame for example) are left out and the calibration is run
   again, when there are at least 8 views and 6 remain; the RMS tooltip says
   how many.

   The lens distortion model is chosen too. More distortion terms always
   fit the views better, even when they only fit noise, and then bend the
   picture's edges, where few corners were seen, in ways the lens does not.
   So three models are compared, simplest first: *k1 k2, square pixels*
   (fx = fy), *k1 k2*, and *k1 k2 k3*. Each is fitted on most of the views
   and scored on the views it did not see (5-fold cross-validation), and the
   simplest model within max(0.05 px, 10%) of the best held-out error is
   used. The RMS tooltip lists each model's held-out error, and the settings
   store the model (``distortion_model``) and its held-out error
   (``holdout_rms_error``). Every model keeps the five coefficients the rest
   of MOSAIC reads; a simpler one holds k3 at zero. With fewer than 8 views
   nothing is compared and *k1 k2 k3* is used, as before. The k3 term only
   earns its place when corners near the picture's edges were seen, so
   cover the edges and corners of the picture. The method comes from the
   4by4k pipeline (``scripts/calib_intrinsics.py``, MIT licence).

   - **RMS error < 0.5 px**: excellent.
   - **0.5 – 1.0 px**: good; acceptable for most pose-estimation tasks.
   - **> 2.0 px**: poor; recapture with better coverage.

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
(above); the **Room (Extrinsics)** page will not solve a camera that hasn't
been calibrated.

1. Open the **Calibrate** tab, **Room (Extrinsics)** page.
2. Choose the board and set its **Squares across**, **down** and
   **Square** size:

   - **ChArUco**: a chessboard with codes in its white squares (also set
     **Marker**). It may be partly out of a camera's view.
   - **Chessboard, one corner square marked**: a plain black-and-white
     chessboard with a mark (a sticker, a dot, a patch of colour) on one
     corner square. A plain chessboard looks the same turned round (a square
     one by a quarter turn), so without the mark two cameras could number
     its corners differently and disagree about where it is; every camera
     numbers them from the marked corner. The whole board, including its
     outer squares, must be in a camera's view for that camera to use it.

   Each shot stores the board's pose in this board's size, so the board
   fields are locked once a shot is captured; click **Clear shots** to start
   over with another board (it also discards any solve made from the old
   shots).
3. Hold the board still somewhere at least two cameras see it and click
   **▶ Capture shot** (the cameras' frames are taken within half a second,
   so a moving board is seen in different places). A green dot under a
   camera means it used the shot; the line under the buttons says why a
   camera that saw something did not (the mark unclear, a corner square cut
   off, the corners fitting poorly, over 2 px). Repeat, moving the board,
   until every camera shares several shots with another camera, and is
   linked to the reference camera through them; 10 to 20 shots at different
   places and tilts is a good start.
4. Pick a **reference camera** (its pose becomes the room's origin, identity
   transform) and click **▶ Solve**. The result table shows, per camera:

   - **Resolved**: no means no chain of shared shots back to the reference
     camera; capture another shot linking it in.
   - **Fit to own view (px)**: how well the board's solved pose fits the
     corners this camera saw (aim for under 1 px).
   - **Agreement with other cameras (mm)**: where this camera puts the
     board in the room, against where the other cameras put it in the same
     shots. This is the check that the calibration is right: a few mm is
     good; over 20 mm (shown in red) means something is wrong, usually a
     square size that does not match the board, a lens calibration that no
     longer fits, or a camera that moved.
5. To define the room's shared reference plane (used by gaze-fusion's
   target-point intersection): lay the board flat on the target surface,
   capture one more shot, then click **Use last shot as plane**.
6. To name a gaze target (a screen, a poster, a toy box) for gaze analysis:
   hold the board flat on it, centred, capture a shot, then click **Add from
   last board shot**. A row appears in **Gaze target regions** with the
   board's size; set its name and the target's real width and height. Repeat
   for each target. Regions are kept as soon as you add, edit or remove one;
   they need no Solve and no **Save to settings**.
7. Click **Save to settings** to persist the per-camera extrinsics and the
   plane. Everything is written to the group's ``settings.json`` when MOSAIC
   closes, and recordings made afterwards carry it in their
   ``session_meta.json``.

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
       **Room (Extrinsics)** procedure above; stays at its default
       identity for any camera that hasn't been resolved yet.
   * - ``extrinsicCalibrated``
     - ``true`` once this camera's extrinsics were successfully resolved by
       the Room (Extrinsics) procedure, distinct from ``calibrated``, which
       only ever means "intrinsics done".
   * - ``imageWidth``, ``imageHeight``, ``imageOffsetX``, ``imageOffsetY``
     - The image the intrinsics were calibrated on: its size and where it sat
       on the sensor (the interview crop, if that camera was in interview
       mode). The principal point only holds for that region, so analysis
       shifts it for recordings made with a different crop. ``-1`` for
       calibrations made before these were recorded.

Room (extrinsic) calibration solves every camera's position and orientation
relative to one shared room origin, needed to combine per-camera 3D
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

``session_meta.json`` also carries a session-wide ``room`` section: the
reference plane from step 5 (``plane_point``/``plane_normal``, 3-vectors in
room coordinates, and ``plane_defined``) and the named gaze target
``regions`` from step 6 (``name``, ``centre``, ``normal``, ``u_axis``,
``width``, ``height``). The Analysis tab's **Multi-Camera Gaze Fusion**
plugin reads these and the per-camera calibration to find what each person
looks at; see :ref:`analysis-api-gaze`.
