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

Pose Estimation
-------------------

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

Face Masking
----------------

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

Speaker Diarization
------------------------

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

Facial Expression
----------------------

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

Multi-Camera Gaze Fusion
------------------------------

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

3D Pose Reconstruction
----------------------------

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

Remote Heart Rate (rPPG)
------------------------------

.. important::

   **EXPERIMENTAL** — research-grade heart-rate estimate only, not a
   medical device, not clinically validated. See
   :doc:`math/remote_heart_rate` for the full accuracy discussion before
   relying on any output.

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

Motion Tracking
--------------------

Run from the Session Browser, not a live Analysis-tab plugin — see
:doc:`user_guide`.

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
