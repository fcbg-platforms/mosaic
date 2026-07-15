Camera calibration
==================

.. contents:: On this page
   :local:
   :depth: 2

Overview
--------

MOSAIC uses OpenCV's checkerboard calibration to compute per-camera **intrinsic
parameters** (focal length, principal point, distortion coefficients).
Results are stored in each camera's :cpp:struct:`mosaic::CalibrationData` field
inside the group's ``settings.json`` and are automatically embedded in
``session_meta.json`` so downstream pose-estimation tools can use them
without manual bookkeeping.

Requirements: build with ``-DMOSAIC_ENABLE_OPENCV=ON``.  The **Calibrate** tab
in the settings sidebar shows a helpful "not available" message otherwise.

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

Calibration procedure
---------------------

1. Open the **Calibrate** tab in the settings sidebar.
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
     - 4 × 4 homogeneous RT relative to camera 0 (identity for cam 0).
       Populated by stereo calibration (planned feature).

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
