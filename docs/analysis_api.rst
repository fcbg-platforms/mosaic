Python Analysis API
======================

.. contents:: On this page
   :local:
   :depth: 2

The ``analysis/`` folder is a separate, ``uv``-managed Python project
(``analysis/pyproject.toml``) from the live-acquisition ``python/``
project — see :doc:`architecture`. It contains six independent offline
plugins, each with a ``run_<plugin>.py`` CLI wrapper
(:doc:`user_guide` covers running them from the app). This page documents
each plugin's importable **library** surface, not the CLI argument
parsing — see each subsection's linked math page for the algorithm behind
its output.

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
