User guide
============

.. contents:: On this page
   :local:
   :depth: 2

This page is a task-oriented walkthrough of MOSAIC's day-to-day workflow:
recording, browsing, calibrating, and running each Analysis-tab plugin. For
file-format/schema reference (what's actually written to disk), see
:doc:`recording` and :doc:`calibration` — this page focuses on *using the
app*, not the on-disk layout.

Recording a session
------------------------

1. Launch MOSAIC and log in to (or create) a research-group profile — see
   :doc:`profiles`.
2. On the **Live** tab, confirm each configured camera's preview is live in
   the monitor grid, and adjust per-camera settings (exposure, gain, ROI,
   trigger mode) in the sidebar if needed.
3. Click **● Record** (or press ``Ctrl+R``). A new timestamped session
   folder is created immediately and the elapsed-time display starts
   counting.
4. Trigger events (keyboard bindings, LSL inlets, parallel-port edges) are
   logged automatically to that session's ``trigger.csv`` for the whole
   duration.
5. Click **■ Stop** (or ``Ctrl+R`` again) to end the session.

See :doc:`recording` for exactly what gets written (``session_meta.json``,
per-camera timestamp CSVs, the post-hoc ``sync_manifest.json``) and why
cameras can end up with slightly different frame counts.

Using the Session Browser
-------------------------------

The Session Browser (accessible from the main window) lists every recorded
session under the profile's configured recording directory, newest first.
For a selected session you can:

- **Play it back** — all cameras' videos in sync, aligned via the session's
  ``sync_manifest.json`` (generated on first playback if it doesn't exist
  yet).
- **Annotate it** — add timestamped labels during playback, exported as CSV.
- **Run Motion tracking** — the one plugin that only runs from here, not the
  Analysis tab (see :ref:`the note below <motion-tracking-note>`).
- See at a glance, via colored badges, which analyses have already been run
  on that session (Pose, Face Masking, Diarization, Expression, Gaze).

Running an Analysis-tab plugin
------------------------------------

Every plugin below (except Motion) lives on the top-level **Analysis** tab,
sitting alongside **Live**. The workflow is always the same shape:

1. Pick a session from the left-hand session list.
2. Pick a plugin from the plugin dropdown.
3. Set that plugin's controls (model/backend/etc — see the tab below for
   each one).
4. Click **Run**. Progress streams into the log view; you can queue another
   session's run while one is in progress — jobs serialize through one
   shared subprocess queue.
5. Once finished (or immediately, if the session already has that plugin's
   output), the results panel loads automatically.

.. tab-set::

   .. tab-item:: Pose (YOLOv8)

      **What it does**: runs a YOLOv8-pose model over every camera's video,
      writing per-frame keypoints to a ``.pose.json`` sidecar next to each
      video.

      **Controls**: a model-size dropdown (shared with the Performance
      tab's own model picker) and a frame-skip spinbox (process every Nth
      frame).

      **Reading the output**: pick a camera, then a keypoint, in the
      results row. The video plays with a skeleton overlay, and a chart
      plots that keypoint's position over time with a playback-synced,
      click-to-seek playhead.

      **Speed / Acceleration**: switch the **Metric** dropdown from
      Position to Speed or Acceleration to see derived kinematics for the
      selected keypoint — see :doc:`math/pose_kinematics` for the math and
      the **Smoothing**/**Scale (mm/px)** controls below. Distance/average-
      speed/max-speed stats and a CSV export are available alongside the
      chart. Kinematics assume a single continuous subject (subject 0) —
      identity isn't tracked frame-to-frame in a multi-animal session.

   .. tab-item:: Face Masking

      **What it does**: produces an anonymized copy of every camera's video
      (faces blurred or boxed) in a sibling ``anonymized/`` folder — the
      originals in ``video/`` are never touched.

      **Controls**: a detection backend (**MediaPipe** — default, best
      recall; **YOLOv8-face** — community checkpoint; **OpenCV DNN** — no
      extra ML framework, weaker on extreme angles), a style (**Blur** or
      **Solid box**), and a frame-skip spinbox (kept low — skipped frames
      reuse the last detected box rather than going unmasked, so raising it
      trades fidelity for speed, not privacy).

      **Reading the output**: the selected camera's anonymized video plays
      directly in the results panel (no overlay/chart — the mask is already
      baked into the video). An **Open output folder** button jumps straight
      to the ``anonymized/`` folder. See :doc:`math/face_masking` for the
      padding/blur-kernel formulas.

   .. tab-item:: Speaker Diarization

      **What it does**: transcribes each microphone's audio with
      **faster-whisper** and, if a Hugging Face token is supplied, labels
      *who* said each segment via **pyannote.audio** speaker diarization.

      **Controls**: Whisper model size (tiny → large-v3; **small** is the
      default speed/accuracy balance), a language dropdown (auto-detect by
      default), a Hugging Face token field (see the token box below),
      optional min/max speaker-count hints, and a "Transcript only" checkbox
      to skip diarization entirely even with a token present.

      **Reading the output**: pick a microphone; the transcript table shows
      Start/End/Speaker/Text rows, and clicking a row seeks audio playback.
      The active row highlights automatically during playback. See
      :doc:`math/speaker_diarization` for the max-overlap speaker-assignment
      rule.

      .. important::

         Diarization needs a **free Hugging Face token** with the terms of
         use accepted for both ``pyannote/speaker-diarization-3.1`` and
         ``pyannote/segmentation-3.0`` (generate a token at
         `huggingface.co/settings/tokens
         <https://huggingface.co/settings/tokens>`_). Without a token,
         transcription still runs — every segment just gets an empty
         speaker label instead of a hard failure.

   .. tab-item:: Facial Expression

      **What it does**: detects faces (MediaPipe FaceLandmarker) and
      classifies each into a dominant emotion, per frame.

      **Controls**: a classifier backend (**Heuristic** — transparent,
      weighted-blendshape rule table, zero extra download; **FER+** — a
      pretrained 8-class ONNX CNN, adds "Contempt"), max-faces, minimum
      detection confidence, and a frame-skip spinbox.

      **Reading the output**: the selected camera's video plays with a
      bounding box + dominant-expression label per detected face. A
      **Blendshape** dropdown drives a chart of that blendshape's score over
      time, and a stats readout shows the %-breakdown across expression
      categories for the run. See :doc:`math/facial_expression` for the
      scoring/softmax formulas. Like Pose kinematics, subject identity isn't
      tracked frame-to-frame.

   .. tab-item:: Multi-Camera Gaze Fusion

      **What it does**: estimates each camera's 3D gaze ray for the subject,
      then triangulates a single fused ray (and, if the room's target plane
      is defined, a target point on that plane) across every camera that saw
      the subject at that instant. Needs
      :ref:`room (extrinsic) calibration <room-extrinsic-calibration>`
      to be completed first — without it, camera positions aren't known in a
      shared room frame and fusion can't run.

      **Controls**: minimum-cameras-to-triangulate (default 2 — below this,
      individual per-camera rays are still recorded, just no fused
      point), minimum detection confidence, and a frame-skip spinbox.

      **Reading the output**: a top-down 3D room view shows each camera's
      position, the defined target plane, the fused ray, and the individual
      per-camera rays (thin, for visually checking triangulation spread).
      The selected camera's video also overlays a face box + gaze-direction
      arrow. Stats show frame count, % triangulated, average residual, and %
      with a valid target point. See :doc:`math/gaze_fusion` for the
      triangulation math and :doc:`math/room_calibration` for how camera
      positions are solved.

.. _motion-tracking-note:

.. note::

   **Motion tracking** runs from the **Session Browser**, not the Analysis
   tab — it operates on the whole session (centroid tracking + heatmap/
   trajectory plots), not a single-camera plugin result view. See
   :doc:`math/motion_tracking` for its detection/assignment math.

Calibration workflow
--------------------------

.. dropdown:: Intrinsic calibration (per camera)
   :open:

   Solves one camera's own lens parameters (focal length, principal point,
   distortion) — needed before that camera's frames can be undistorted or
   used in any 3D computation, including gaze fusion.

   1. Open the **Calibrate** sidebar tab, then the **Intrinsics** inner tab.
   2. Select the camera, set checkerboard **Cols**/**Rows**/**Square size**.
   3. Move the checkerboard through varied positions/angles/distances,
      clicking **Capture frame** on each good view — aim for 20–30 views.
   4. Click **▶ Calibrate**; check the reported RMS error (< 0.5 px is
      excellent, > 2.0 px means recapture with better coverage).
   5. Click **Save calibration to settings**.

   Full detail, including the checkerboard-printing recommendations and the
   exact ``CalibrationData`` fields written, is in :doc:`calibration`.

.. _room-extrinsic-calibration:

.. dropdown:: Room (extrinsic) calibration (all cameras at once)

   Solves every camera's position/orientation in one shared room coordinate
   frame — needed to combine per-camera 3D signals (like gaze rays) across
   cameras. Uses a **ChArUco board** (not the plain checkerboard from
   intrinsics) captured simultaneously by multiple cameras at once, since it
   tolerates partial views when different cameras only see part of the board
   from their own angle.

   1. Complete intrinsic calibration for every camera you want included
      first — extrinsic solving needs each camera's own lens parameters.
   2. Open the **Calibrate** sidebar tab, then the **Room (Extrinsics)**
      inner tab. Each configured camera gets its own live-thumbnail panel.
   3. Hold the ChArUco board somewhere visible to two or more cameras at
      once and click **▶ Capture shot**. A found/not-found dot updates per
      camera. Repeat, moving the board through overlapping camera pairs,
      until every camera has a path of shared shots back to camera 0
      (the fixed room-origin reference) — 8+ varied shots is a reasonable
      starting point for 6 cameras.
   4. Click **▶ Solve**. The result table shows, per camera, whether it
      resolved and its reprojection RMS (px) — a camera with no shared-shot
      path back to camera 0 is reported unresolved rather than silently
      wrong.
   5. Lay the board flat on the surface you want as the gaze-fusion target
      plane (a table, a screen), capture one more shot, then click
      **Use last shot as plane**.
   6. Click **Save to settings**.

   See :doc:`math/room_calibration` for the pose-graph/quaternion-averaging
   math behind the solve step.

Tips and troubleshooting
------------------------------

.. grid:: 1 1 2 2
   :gutter: 2

   .. grid-item-card:: 🔑  Hugging Face token gating

      Diarization's underlying pyannote models are gated on Hugging Face —
      you must accept both models' terms of use on huggingface.co *before*
      a generated token will work, not just create the token. A token that
      hasn't accepted the terms fails with an authentication error, not a
      missing-token warning.

   .. grid-item-card:: 📦  FER+ download trap

      The FER+ model file is served over Git LFS upstream. If a download
      ever produces a suspiciously small file (~130 bytes instead of ~35
      MB), it's an LFS pointer stub, not the real model — MOSAIC's
      downloader already guards against this with a sha256 check, but it's
      worth knowing if you ever fetch the model manually.

   .. grid-item-card:: 🖥️  GPU / CPU auto-selection

      Pose, diarization, and gaze fusion all auto-select CUDA if a working
      GPU is available, falling back to CPU otherwise — no manual device
      flag needed. FER+ and face-mask detectors run CPU-only by design
      (the models are small enough that GPU wouldn't meaningfully help).

   .. grid-item-card:: 🧑‍🤝‍🧑  Subject identity across frames

      No plugin tracks *which* physical subject is which across frames in
      a multi-subject session — "subject 0" just means "first detection in
      that frame." Kinematics/expression stats are only meaningful for
      single-subject sessions today.
