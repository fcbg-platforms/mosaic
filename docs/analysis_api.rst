Python Analysis API
======================

.. contents:: On this page
   :local:
   :depth: 2

The ``analysis/`` folder is a separate, ``uv``-managed Python project
(``analysis/pyproject.toml``) from the live-acquisition ``python/``
project — see :doc:`architecture`. It contains eight independent offline
Analysis-tab plugins, each with a ``run_<plugin>.py`` CLI wrapper
(:doc:`user_guide` covers running them from the app), plus a ninth
supporting package (:ref:`analysis-api-transcribe`) used by the
**Real-time** tab's live captions, not by any post-hoc plugin. This page
documents each package's importable **library** surface, not the CLI
argument parsing — see each subsection's linked math page for the
algorithm behind its output.

.. grid:: 2 2 3 3
   :gutter: 3

   .. grid-item-card:: 🚶 Pose Estimation
      :link: analysis-api-pose
      :link-type: ref

      2D COCO keypoints via YOLOv8-pose — the foundational signal every
      derived plugin below builds on.

   .. grid-item-card:: 🫥 Face Masking
      :link: analysis-api-facemask
      :link-type: ref

      Detect and blur/box faces for anonymized sharing outside the lab.

   .. grid-item-card:: 🗣️ Speaker Diarization
      :link: analysis-api-diarize
      :link-type: ref

      faster-whisper transcription + pyannote speaker turns, merged by
      max overlap.

   .. grid-item-card:: 🙂 Facial Expression
      :link: analysis-api-expression
      :link-type: ref

      3 backends: a transparent blendshape heuristic, FER+, and real FACS
      Action Units via py-feat.

   .. grid-item-card:: 🎯 Multi-Camera Gaze Fusion
      :link: analysis-api-gaze
      :link-type: ref

      Per-camera 3D gaze rays, triangulated into one fused ray + target
      point.

   .. grid-item-card:: 🧍 3D Pose Reconstruction
      :link: analysis-api-pose3d
      :link-type: ref

      Multi-view DLT triangulation and cross-camera person tracking, from
      the Pose plugin's own 2D output.

   .. grid-item-card:: ❤️ Remote Heart Rate
      :link: analysis-api-rppg
      :link-type: ref

      Camera-based pulse-rate estimation (rPPG). **Experimental.**

   .. grid-item-card:: 🐭 Motion Tracking
      :link: analysis-api-motion
      :link-type: ref

      Background-subtraction centroid tracking, run from the Session
      Browser.

   .. grid-item-card:: 💬 Live Transcription
      :link: analysis-api-transcribe
      :link-type: ref

      Rolling-buffer streaming speech-to-text behind the Real-time tab's
      captions.

.. _analysis-api-pose:

Pose Estimation
-------------------

YOLOv8-pose detection over a session's videos, producing per-frame,
per-subject 2D keypoints in COCO format. This is the foundational 2D
signal every derived plugin consumes: :doc:`math/pose_kinematics`'s
speed/acceleration, and the 3D Pose Reconstruction plugin below.

.. code-block:: python

   from pose import HumanPoseEstimator

   estimator = HumanPoseEstimator(model_name="yolov8n-pose.pt")
   result = estimator.infer(frame_bgr, frame_index=0, timestamp_ns=0, camera_index=0)

.. automodule:: pose
   :no-members:

.. autosummary::
   :nosignatures:

   HumanPoseEstimator
   PoseResult
   SubjectPose

.. autoclass:: pose.HumanPoseEstimator
   :members:
   :undoc-members:

.. autoclass:: pose.PoseResult
   :members:

.. autoclass:: pose.SubjectPose
   :members:

See :doc:`math/pose_kinematics` for the Speed/Acceleration math applied to
this plugin's output (implemented in C++, not here — see
:cpp:func:`mosaic::compute_kinematics`).

.. tip::

   ``run_pose.py`` has two operating modes: **session mode**
   (``--session <dir>``, the common post-processing path used by the
   Analysis tab) and **pipe mode** (``--pipe``, base64-JPEG frames over
   stdin/stdout), which is how MOSAIC's live Real-time tab drives the same
   model internally.

.. _analysis-api-facemask:

Face Masking
----------------

Anonymizes a session's videos by detecting and blurring/boxing faces,
writing the result into a sibling ``anonymized/`` folder — the originals
are never touched. Three interchangeable detector backends
(:class:`~facemask.MediaPipeFaceDetector`, default;
:class:`~facemask.YoloFaceDetector`; :class:`~facemask.OpenCVDnnFaceDetector`,
YuNet) share one ``detect() -> list[Box]`` interface.

.. code-block:: python

   from facemask import make_detector, expand_and_clip, apply_mask

   detector = make_detector("mediapipe", model=None, conf_threshold=0.5)
   boxes = [expand_and_clip(b, 0.25, frame_w, frame_h) for b in detector.detect(frame_bgr)]
   masked = apply_mask(frame_bgr, boxes, style="blur")

.. automodule:: facemask
   :no-members:

``facemask.Box`` is a plain type alias, ``tuple[float, float, float, float]``
(``x1, y1, x2, y2`` pixel coordinates) — the shape every detector's
``detect()`` and both geometry functions below use.

.. autosummary::
   :nosignatures:

   FaceDetector
   MediaPipeFaceDetector
   YoloFaceDetector
   OpenCVDnnFaceDetector
   make_detector
   expand_and_clip
   apply_mask

.. autoclass:: facemask.FaceDetector
   :members:

.. autoclass:: facemask.MediaPipeFaceDetector
   :members:

.. autoclass:: facemask.YoloFaceDetector
   :members:

.. autoclass:: facemask.OpenCVDnnFaceDetector
   :members:

.. autofunction:: facemask.make_detector

.. autofunction:: facemask.expand_and_clip

.. autofunction:: facemask.apply_mask

See :doc:`math/face_masking`.

.. _analysis-api-diarize:

Speaker Diarization
------------------------

Transcribes each microphone's audio with faster-whisper, diarizes speaker
turns with pyannote.audio, then assigns each transcript segment to
whichever diarization turn overlaps it most
(:func:`~diarize.assign_speakers`) — the standard WhisperX-style recipe.
Diarization is optional: without a Hugging Face token, transcription still
runs and every segment's speaker is left ``None``.

.. important::

   pyannote's diarization models are **gated** on Hugging Face — you must
   accept both ``pyannote/speaker-diarization-3.1`` and
   ``pyannote/segmentation-3.0``'s terms of use and generate an access
   token before diarization (not just transcription) will work. See
   :doc:`user_guide` for where to configure the token in the app.

.. code-block:: python

   from diarize import resolve_device, load_whisper_model, transcribe_audio

   device = resolve_device(device_arg=None)
   model = load_whisper_model("small", device)
   segments, detected_language = transcribe_audio(model, audio_path, language=None)

.. automodule:: diarize
   :no-members:

.. autosummary::
   :nosignatures:

   resolve_device
   load_whisper_model
   transcribe_audio
   load_diarization_pipeline
   diarize_audio
   assign_speakers

.. autofunction:: diarize.resolve_device

.. autofunction:: diarize.load_whisper_model

.. autofunction:: diarize.transcribe_audio

.. autofunction:: diarize.load_diarization_pipeline

.. autofunction:: diarize.diarize_audio

.. autofunction:: diarize.assign_speakers

.. autoclass:: diarize.WhisperSegment
   :members:

.. autoclass:: diarize.DiarizationTurn
   :members:

.. autoclass:: diarize.TranscriptSegment
   :members:

See :doc:`math/speaker_diarization`.

.. _analysis-api-expression:

Facial Expression
----------------------

Detects faces and their MediaPipe blendshapes per frame, then classifies
a dominant expression via one of three interchangeable backends: a
transparent, dependency-free weighted-blendshape heuristic (default), a
pretrained FER+ ONNX model, or py-feat for real, continuous FACS Action
Unit intensities (see the dedicated subsection below).

.. code-block:: python

   from expression import MediaPipeExpressionDetector, classify_expression, BLENDSHAPE_NAMES

   detector = MediaPipeExpressionDetector()
   faces = detector.detect(frame_bgr)
   label, score = classify_expression(BLENDSHAPE_NAMES, faces[0].blendshape_scores)

.. automodule:: expression
   :no-members:

.. autosummary::
   :nosignatures:

   classify_expression
   MediaPipeExpressionDetector
   crop_bbox
   FerPlusClassifier

.. autofunction:: expression.classify_expression

.. py:data:: expression.CATEGORIES
   :type: list[str]
   :value: ["Neutral", "Happy", "Sad", "Surprised", "Angry", "Disgusted", "Fearful"]

   The 7 basic-emotion categories the heuristic backend classifies into,
   in argmax tie-break order (``"Neutral"`` listed first — see
   :func:`~expression.classify_expression`'s Notes).

.. py:data:: expression.CATEGORY_WEIGHTS
   :type: dict[str, dict[str, float]]

   ``{category: {blendshape_name: weight}}``. Weighted MEAN (not sum) is
   taken per category at classification time, so a category listing 6
   blendshapes isn't unfairly favored over one listing 2 — every
   category's score stays comparable in ``[0, 1]`` regardless of how many
   shapes it references.

.. autoclass:: expression.MediaPipeExpressionDetector
   :members:

.. autoclass:: expression.FaceExpression
   :members:

.. py:data:: expression.BLENDSHAPE_NAMES
   :type: list[str]

   The standard ARKit-style blendshape category names MediaPipe's
   ``FaceLandmarker`` outputs when ``output_face_blendshapes=True``. Score
   lookup is always by category **name** against this list (never
   positional), so a future mediapipe version reordering its output
   categories can't silently misalign names/scores.

.. autofunction:: expression.crop_bbox

.. autoclass:: expression.FerPlusClassifier
   :members:

.. py:data:: expression.FERPLUS_LABELS
   :type: list[str]

   Official FER+ label order (index 0-7). Verified against both the
   ``onnx/models`` model card and the upstream FERPlus training repo's CSV
   column order — see the module's own docstring for the two-source
   cross-check this pinned down.

**py-feat backend** (:mod:`expression.pyfeat`) — the third, most detailed
backend: real FACS Action Units, not just a dominant-emotion label.

.. autoclass:: expression.pyfeat.PyFeatClassifier
   :members:

.. py:data:: expression.pyfeat.AU_NAMES
   :type: list[str]

   The 20 py-feat/``Detectorv1`` xgb-head Action Units (verified against
   ``feat/pretrained.py``'s ``AU_LANDMARK_MAP["Feat"]``). Values are a
   continuous ``[0, 1]`` calibrated probability, **not** the classic FACS
   0–5 intensity scale.

.. autofunction:: expression.pyfeat._fex_row_to_result

.. important::

   ``import feat`` unconditionally pulls in ``torchcodec`` at module load
   time, which needs a torchcodec-compatible FFmpeg (versions 4–8, a
   shared/DLL build) discoverable on ``PATH`` at runtime — even though
   this backend never touches video I/O. See the module's own docstring
   and :doc:`math/facial_expression`'s recommendations for the full
   diagnosis if this backend fails to construct.

See :doc:`math/facial_expression`.

.. _analysis-api-gaze:

Multi-Camera Gaze Fusion
------------------------------

For each camera that sees a face, solves a real (not weak-perspective) 3D
head pose via ``cv2.solvePnP``, perturbs it by a small iris-offset
heuristic into one camera-local gaze ray, transforms every contributing
camera's ray into shared room coordinates, and triangulates them into one
fused ray and (if a target plane is calibrated) a target point.

.. code-block:: python

   from gaze import transform_ray_to_room, closest_point_of_rays

   rays_room = [transform_ray_to_room(origin, direction, cam.extrinsic_rt)
                for origin, direction, cam in per_camera_rays]
   origins, directions = zip(*rays_room)
   fused_point, residual_rms = closest_point_of_rays(origins, directions)

.. automodule:: gaze
   :no-members:

.. autosummary::
   :nosignatures:

   camera_ray_from_pose
   transform_ray_to_room
   closest_point_of_rays
   ray_plane_intersection
   MediaPipeGazeEstimator3D

.. autofunction:: gaze.camera_ray_from_pose

.. autofunction:: gaze.transform_ray_to_room

.. autofunction:: gaze.closest_point_of_rays

.. autofunction:: gaze.ray_plane_intersection

.. autoclass:: gaze.MediaPipeGazeEstimator3D
   :members:

.. autoclass:: gaze.FaceGazeSample
   :members:

See :doc:`math/gaze_fusion` and :doc:`math/room_calibration` (this plugin
consumes the room/extrinsic calibration solved there).

.. _analysis-api-pose3d:

3D Pose Reconstruction
----------------------------

Triangulates each camera's already-computed 2D COCO keypoints (from the
Pose plugin above) into one 3D skeleton per detected person, per frame.
Three stages: multi-view DLT triangulation with one-shot reprojection-
error outlier rejection (:func:`~pose3d.triangulate_with_rejection`),
cross-camera person association by reusing that same triangulation cost
as a matching score (:func:`~pose3d.cluster_people`), and greedy
nearest-centroid tracking across frames (:class:`~pose3d.PersonTracker3D`).

.. important::

   Needs the Pose plugin to have already been run on at least 2 cameras in
   the session (for the 2D keypoints) **and** room/extrinsic calibration
   to have been solved (:doc:`math/room_calibration`) — running this
   plugin against a session missing either prerequisite fails with a
   clear error rather than a fabricated result.

.. automodule:: pose3d
   :no-members:

.. autosummary::
   :nosignatures:

   CameraGeom
   invert_rt
   normalize_point
   triangulate_point_dlt
   reproject_error_px
   triangulate_with_rejection
   PersonObservation
   pairwise_cost
   match_camera_pair
   cluster_people
   PersonTracker3D

.. autoclass:: pose3d.CameraGeom
   :members:

.. autofunction:: pose3d.invert_rt

.. autofunction:: pose3d.normalize_point

.. autofunction:: pose3d.projection_matrix

.. autofunction:: pose3d.triangulate_point_dlt

.. autofunction:: pose3d.project_point_px

.. autofunction:: pose3d.reproject_error_px

.. autoclass:: pose3d.TriangulationResult
   :members:

.. autofunction:: pose3d.triangulate_with_rejection

.. autoclass:: pose3d.PersonObservation
   :members:

.. autofunction:: pose3d.pairwise_cost

.. autofunction:: pose3d.match_camera_pair

.. autofunction:: pose3d.cluster_people

.. autoclass:: pose3d.TrackedPerson3D
   :members:

.. autoclass:: pose3d.PersonTracker3D
   :members:

See :doc:`math/pose3d_reconstruction`. Consumes the room/extrinsic
calibration solved in :doc:`math/room_calibration`, and the Pose plugin's
own per-camera ``.pose.json`` output (:doc:`math/pose_kinematics`'s data
source) as its 2D input.

.. _analysis-api-rppg:

Remote Heart Rate (rPPG)
------------------------------

.. important::

   **EXPERIMENTAL** — research-grade heart-rate estimate only, not a
   medical device, not clinically validated. See
   :doc:`math/remote_heart_rate` for the full accuracy discussion before
   relying on any output.

Extracts a forehead/cheek skin-color signal per frame
(:class:`~rppg.MediaPipeFaceRoiExtractor`), combines its RGB channels into
one pulse signal via a selectable backend (naive Green, or the more
motion-robust CHROM/POS chrominance methods — POS is the default), then
bandpass-filters and Welch-periodogram-analyzes each sliding time window
to estimate BPM and a pulse-SNR quality score. Deliberately offers **no**
frame-skip option, unlike every sibling plugin — skipping frames would
downsample the pulse signal itself below what Nyquist needs for the
physiological frequency band.

.. code-block:: python

   from rppg import BACKENDS, bandpass_filter, estimate_hr_welch

   pulse_signal = BACKENDS["pos"](rgb_means)          # (N, 3) -> (N,)
   filtered = bandpass_filter(pulse_signal, fs=frame_rate_hz)
   bpm, snr_db = estimate_hr_welch(filtered, fs=frame_rate_hz)

.. automodule:: rppg
   :no-members:

.. autosummary::
   :nosignatures:

   MediaPipeFaceRoiExtractor
   FaceRoiSample
   normalize_channels
   green_signal
   chrom_signal
   pos_signal
   bandpass_filter
   estimate_hr_welch
   median_smooth

.. autoclass:: rppg.MediaPipeFaceRoiExtractor
   :members:

.. autoclass:: rppg.FaceRoiSample
   :members:

.. autofunction:: rppg.normalize_channels

.. autofunction:: rppg.green_signal

.. autofunction:: rppg.chrom_signal

.. autofunction:: rppg.pos_signal

.. py:data:: rppg.BACKENDS
   :type: dict[str, typing.Callable]

   ``{"green": green_signal, "chrom": chrom_signal, "pos": pos_signal}`` —
   the backend-name → pure-function dispatch table ``run_rppg.py``'s
   ``--backend`` argument resolves against.

.. autofunction:: rppg.bandpass_filter

.. autofunction:: rppg.estimate_hr_welch

.. autofunction:: rppg.median_smooth

See :doc:`math/remote_heart_rate`.

.. _analysis-api-motion:

Motion Tracking
--------------------

Run from the Session Browser, not a live Analysis-tab plugin — see
:doc:`user_guide`. Background-subtraction blob detection
(:class:`~motion.CentroidTracker`) plus greedy nearest-centroid tracking
across frames, independent of the Pose plugin's keypoint-based approach —
see :doc:`math/motion_tracking` for an explicit contrast between the two.

.. code-block:: python

   from motion import CentroidTracker

   tracker = CentroidTracker(mm_per_px=1.0)
   tracks = tracker.update(frame_bgr, timestamp_ns=0, fps=30.0)

.. automodule:: motion
   :no-members:

.. autosummary::
   :nosignatures:

   CentroidTracker
   Track
   draw_tracks
   generate_heatmap
   generate_trajectory_plot
   generate_velocity_histogram

.. autoclass:: motion.CentroidTracker
   :members:

.. autoclass:: motion.Track
   :members:

.. autofunction:: motion.draw_tracks

.. autofunction:: motion.generate_heatmap

.. autofunction:: motion.generate_trajectory_plot

.. autofunction:: motion.generate_velocity_histogram

See :doc:`math/motion_tracking`.

.. _analysis-api-transcribe:

Live Transcription (Real-time tab)
----------------------------------------

Not a post-hoc Analysis-tab plugin — this package backs the **Real-time**
tab's live-captions panel (``analysis/run_live_transcribe.py``, a
persistent subprocess started by :cpp:class:`mosaic::TranscriptWorker`; see
:doc:`user_guide`). Documented here because it's real, importable,
independently-testable library code, not because it fits the "run a
session, get a result file" shape every plugin above does.

.. code-block:: python

   from transcribe import pcm16_to_mono_float32, resample_to_16k, confirm_segments

   mono = resample_to_16k(pcm16_to_mono_float32(pcm_bytes, channels=2), source_rate_hz=48000)
   # ... run whisper over the rolling buffer, then split its segments ...
   confirmed, tentative_text, watermark_sec = confirm_segments(
       segments, buffer_duration_sec=8.0, trailing_margin_sec=1.0)

.. automodule:: transcribe
   :no-members:

.. autosummary::
   :nosignatures:

   Segment
   confirm_segments
   trim_buffer_samples
   pcm16_to_mono_float32
   resample_to_16k

.. autoclass:: transcribe.Segment
   :members:

.. autofunction:: transcribe.confirm_segments

.. autofunction:: transcribe.trim_buffer_samples

.. autofunction:: transcribe.pcm16_to_mono_float32

.. autofunction:: transcribe.resample_to_16k

**Trailing-margin confirmation.** Whisper (``tiny`` model, by default) is
re-run over the *entire* rolling audio buffer on every pass rather than
incrementally. A segment is confirmed — final, never revised again — once
its end lies at least ``TRAILING_MARGIN_SEC`` before the buffer's current
end, giving it a margin of trailing audio context on both the previous
pass and this one; everything after that point is "tentative" text,
replaced wholesale each pass. Confirmed audio is then trimmed off the
buffer's front so growth stays bounded. This intentionally skips more
elaborate cross-pass textual-agreement ("LocalAgreement-n") policies some
streaming-ASR projects use — VAD-anchored segment boundaries are already
stable in practice for the confirmed prefix, and the simpler rule is
sufficient for a ``tiny``-model live-captions v1.
