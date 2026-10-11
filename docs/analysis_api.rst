Python Analysis API
======================

.. contents:: On this page
   :local:
   :depth: 2

The ``analysis/`` folder is a separate, ``uv``-managed Python project
(``analysis/pyproject.toml``) from the live-acquisition ``python/``
project (see :doc:`architecture`). It contains eight independent offline
Analysis-tab plugins, each with a ``run_<plugin>.py`` CLI wrapper
(:doc:`user_guide` covers running them from the app), plus a ninth
supporting package (:ref:`analysis-api-transcribe`) used by the
**Real-time** tab's live captions, not by any post-hoc plugin. This page
documents each package's importable **library** surface, not the CLI
argument parsing; see each subsection's linked math page for the
algorithm behind its output.

.. grid:: 2 2 3 3
   :gutter: 3

   .. grid-item-card:: 🚶 Pose Estimation
      :link: analysis-api-pose
      :link-type: ref

      2D COCO keypoints via YOLOv8-pose: the foundational signal every
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

   .. grid-item-card:: 😉 Face Dynamics
      :link: analysis-api-face-dynamics
      :link-type: ref

      Blinks, Duchenne smiles, brow raises, expressivity, nods and shakes
      from one camera's video.

   .. grid-item-card:: 👀 Eye Contact
      :link: analysis-api-eye-contact
      :link-type: ref

      Looking at the partner or away, while speaking or listening, from one
      camera's gaze.

   .. grid-item-card:: 💬 Conversation Timing
      :link: analysis-api-conversation
      :link-type: ref

      Turns, response times, pauses, overlaps and backchannels from the
      audio and the face on camera.

   .. grid-item-card:: 📄 Session report
      :link: analysis-api-report
      :link-type: ref

      Every analysis of a session in one HTML page and one summary row;
      combined across sessions for statistics.

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
this plugin's output (implemented in C++, not here; see
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
writing the result into a sibling ``anonymized/`` folder; the originals
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
(``x1, y1, x2, y2`` pixel coordinates): the shape every detector's
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
(:func:`~diarize.assign_speakers`), the standard WhisperX-style recipe.
Diarization is optional: without a Hugging Face token, transcription still
runs and every segment's speaker is left ``None``.

.. important::

   pyannote's diarization models are **gated** on Hugging Face: you must
   accept both ``pyannote/speaker-diarization-3.1`` and
   ``pyannote/segmentation-3.0``'s terms of use and generate an access
   token before diarization (not just transcription) will work. See
   :doc:`user_guide` for where to configure the token in the app.

.. code-block:: python

   from diarize import resolve_whisper_device, load_whisper_model, transcribe_audio

   device = resolve_whisper_device(device_arg=None)
   model = load_whisper_model("small", device)
   segments, detected_language = transcribe_audio(model, audio_path, language=None)

.. automodule:: diarize
   :no-members:

.. autosummary::
   :nosignatures:

   resolve_device
   resolve_whisper_device
   load_whisper_model
   transcribe_audio
   load_diarization_pipeline
   diarize_audio
   assign_speakers

.. autofunction:: diarize.resolve_device

.. autofunction:: diarize.resolve_whisper_device

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
   in argmax tie-break order (``"Neutral"`` listed first; see
   :func:`~expression.classify_expression`'s Notes).

.. py:data:: expression.CATEGORY_WEIGHTS
   :type: dict[str, dict[str, float]]

   ``{category: {blendshape_name: weight}}``. Weighted MEAN (not sum) is
   taken per category at classification time, so a category listing 6
   blendshapes isn't unfairly favored over one listing 2: every
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
   column order. See the module's own docstring for the two-source
   cross-check this pinned down.

**py-feat backend** (:mod:`expression.pyfeat`): the third, most detailed
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
   shared/DLL build) discoverable on ``PATH`` at runtime, even though
   this backend never touches video I/O. See the module's own docstring
   and :doc:`math/facial_expression`'s recommendations for the full
   diagnosis if this backend fails to construct.

See :doc:`math/facial_expression`.

.. _analysis-api-gaze:

Multi-Camera Gaze Fusion
------------------------------

Where every subject looks, in 3D, across all calibrated cameras: whose face,
which named region, which point on the calibrated plane, or which point in
space. Faces are found with YuNet and landmarked by MediaPipe on crops; head
pose is metric (MediaPipe's canonical face against the real calibration,
fitted jointly across cameras with a per-subject face scale); a geometric
eyeball model turns each iris into an eye rotation; every camera's view of
each eye is fused robustly; subjects are tracked and named; gaze is
smoothed without lag and cast against the targets. See
:doc:`math/gaze_fusion` for the method and its limits.

.. code-block:: text

   python run_gaze_fusion.py --session DIR [--skip 2] [--subjects N]
                             [--min-cameras 1] [--min-confidence 0.6]
                             [--camera N] [--raw] [--no-render] [--refresh]

``--camera N`` uses one camera on its own (no room calibration needed);
``--raw`` ignores ``synced/``; ``--refresh`` ignores the landmark cache.

**Outputs.**

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - File
     - Contents
   * - ``gaze_fusion.json``
     - Schema ``mosaic-gaze-fusion-v2``, read by the Analysis tab. Top level:
       ``source`` (``synced`` or ``raw``), ``fps``, ``analysed_every``,
       ``subjects`` (``id``, ``name``, ``face_scale``), ``regions``,
       ``plane``, ``cameras``, ``annotated_videos``. Per analysed tick in
       ``frames``: ``tick``, ``timestamp_ns``, ``video_frame_index`` and per
       subject ``origin``, ``direction``, ``point`` (mm, room frame),
       ``target`` (``type``, ``label``, ``subject``, ``distance_mm``),
       ``mutual``, ``confidence``, ``uncertainty_deg``, ``n_cameras`` and
       ``per_camera`` (``camera``, ``face_box_px``, ``direction``,
       ``weight``).
   * - ``gaze_fusion/gaze_fusion.csv``
     - One row per analysed tick and subject, the same values flat.
   * - ``gaze_fusion/summary.json``
     - Per subject: seconds seen, seconds with a gaze, and seconds looking
       at each target; mutual gaze per pair.
   * - ``gaze_fusion/video_N.gaze.mp4``
     - Each camera's video (frame *k* = tick *k*) with every subject's face
       box, name, projected gaze ray, gaze point, a 1.5 s trail and their
       target (``S1 -> S2``, ``S1 <-> S2`` for mutual gaze).
   * - ``gaze_fusion/room_topdown.mp4``
     - The room from above: cameras, regions, subjects, gaze rays and a
       heat layer of gaze points that builds up over the recording.
   * - ``gaze_fusion/heatmap_<subject>.png``
     - Where each subject's gaze landed, from above.
   * - ``gaze_fusion/landmarks_camN.npz``
     - Cached landmarks, so a re-run with the same settings skips face
       finding (renaming subjects or editing regions then takes seconds).

**Targets.** The plane and named regions come from the room calibration
snapshotted in ``session_meta.json``. An optional ``gaze_targets.json`` in
the session folder overrides the regions and renames subjects:

.. code-block:: json

   {
     "subjects": {"S1": "Child", "S2": "Parent"},
     "regions": [
       {"name": "screen", "centre": [0, -200, 1800], "normal": [0, 0, -1],
        "u_axis": [1, 0, 0], "width": 600, "height": 340}
     ]
   }

.. code-block:: python

   import json

   data = json.load(open("gaze_fusion.json"))
   for frame in data["frames"]:
       for s in frame["subjects"]:
           if s["mutual"]:
               print(frame["timestamp_ns"], s["name"], "and", s["target"]["label"])

.. automodule:: gaze
   :no-members:

.. automodule:: gaze.ray_math
   :members:

.. automodule:: gaze.canonical
   :members: CanonicalFace, load_canonical_face, parse_geometry_metadata

.. automodule:: gaze.face_detect
   :members: FaceFinder, FaceLandmarks, landmark_ids, square_crop, cut_crop, crop_to_frame

.. automodule:: gaze.head_pose
   :members:

.. automodule:: gaze.eye_model
   :members: estimate_eye, iris_on_sphere, visual_axis, limit_eye_rotation, eye_openness, eyeball_centre_head, EyeEstimate

.. automodule:: gaze.fusion
   :members: GazeRig, FaceObs, Entry, assign_subjects, apply_subject_scale, drop_duplicates, finalize

.. automodule:: gaze.filters
   :members:

.. automodule:: gaze.targets
   :members: Region, TargetHit, cast_gaze, apply_min_dwell

.. automodule:: gaze.timeline
   :members:

.. automodule:: gaze.io_v2
   :members: load_targets, write_results, write_csv, summarise

.. automodule:: gaze.render
   :members: RenderData, SubjectTrack, build_render_data, render_camera_video, render_topdown, write_heatmaps

See :doc:`math/room_calibration` for the room calibration this plugin uses.

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
   to have been solved (:doc:`math/room_calibration`). Running this
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

   **EXPERIMENTAL**: research-grade heart-rate estimate only, not a
   medical device, not clinically validated. See
   :doc:`math/remote_heart_rate` for the full accuracy discussion before
   relying on any output.

Extracts a forehead/cheek skin-color signal per frame
(:class:`~rppg.MediaPipeFaceRoiExtractor`), combines its RGB channels into
one pulse signal via a selectable backend (naive Green, or the more
motion-robust CHROM/POS chrominance methods; POS is the default), then
bandpass-filters and Welch-periodogram-analyzes each sliding time window
to estimate BPM and a pulse-SNR quality score. Deliberately offers **no**
frame-skip option, unlike every sibling plugin: skipping frames would
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
   hrv.analyse
   hrv.pos_overlap_add
   hrv.detect_beats
   hrv.refine_with_template
   hrv.clean_intervals
   hrv.timing_jitter
   hrv.hrv_time_domain
   hrv.hrv_frequency_domain

Beat-to-beat timing and HRV (:mod:`rppg.hrv`) take the same per-frame
colour through a continuous POS pulse wave, matched-filter beat timing and
artifact cleaning; the two halves of the face give the timing noise:

.. code-block:: python

   from rppg import hrv

   r = hrv.analyse(times_s, rgb, fs=50.0, halves=(rgb_left, rgb_right))
   r["hrv"]["rmssd_ms"], r["hrv"]["rmssd_corrected_ms"], r["hrv"]["timing_jitter_ms"]

.. autofunction:: rppg.hrv.analyse

.. autofunction:: rppg.hrv.pos_overlap_add

.. autofunction:: rppg.hrv.detect_beats

.. autofunction:: rppg.hrv.refine_with_template

.. autofunction:: rppg.hrv.clean_intervals

.. autofunction:: rppg.hrv.timing_jitter

.. autofunction:: rppg.hrv.hrv_time_domain

.. autofunction:: rppg.hrv.hrv_frequency_domain

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

   ``{"green": green_signal, "chrom": chrom_signal, "pos": pos_signal}``:
   the backend-name → pure-function dispatch table ``run_rppg.py``'s
   ``--backend`` argument resolves against.

.. autofunction:: rppg.bandpass_filter

.. autofunction:: rppg.estimate_hr_welch

.. autofunction:: rppg.median_smooth

See :doc:`math/remote_heart_rate`.

.. _analysis-api-face-dynamics:

Face Dynamics
-------------

Counts what a face does: blinks and long closures from the eye aspect ratio
normalised by the person's own open-eye baseline, smiles (and Duchenne
smiles) and brow raises from MediaPipe blendshapes, an expressivity index,
and head nods and shakes from the head-pose angles. The rules are pure numpy
(:mod:`face_dynamics.metrics`); :class:`~face_dynamics.extract.FaceTracker`
supplies the per-frame measurements. See :doc:`math/face_dynamics`.

.. code-block:: python

   from face_dynamics import metrics as fm

   open_l = fm.openness(ear_left, times_s)          # ~1 open, 0 closed
   open_r = fm.openness(ear_right, times_s)
   events = fm.detect_blinks(open_l, open_r, times_s)
   events += fm.detect_expression_events(blendshapes, times_s)
   events += fm.detect_head_gestures(fm.smooth(yaw, times_s), fm.smooth(pitch, times_s), times_s)

.. automodule:: face_dynamics
   :no-members:

.. autosummary::
   :nosignatures:

   metrics.eye_aspect_ratio
   metrics.openness
   metrics.detect_blinks
   metrics.perclos
   metrics.detect_expression_events
   metrics.expressivity
   metrics.detect_head_gestures
   metrics.smooth
   metrics.angular_speed
   metrics.summarise
   extract.FaceTracker
   extract.head_angles

.. autoclass:: face_dynamics.metrics.Event
   :members:

.. autofunction:: face_dynamics.metrics.eye_aspect_ratio

.. autofunction:: face_dynamics.metrics.openness

.. autofunction:: face_dynamics.metrics.detect_blinks

.. autofunction:: face_dynamics.metrics.perclos

.. autofunction:: face_dynamics.metrics.detect_expression_events

.. autofunction:: face_dynamics.metrics.expressivity

.. autofunction:: face_dynamics.metrics.detect_head_gestures

.. autofunction:: face_dynamics.metrics.smooth

.. autofunction:: face_dynamics.metrics.angular_speed

.. autofunction:: face_dynamics.metrics.summarise

.. autoclass:: face_dynamics.extract.FaceTracker
   :members: measure, close

.. autofunction:: face_dynamics.extract.head_angles

.. _analysis-api-eye-contact:

Eye Contact
-----------

Gaze per frame from one camera with the 3D gaze plugin's eyeball model
(:class:`~eye_contact.gaze.GazeModel`), the partner's direction as the mode of
the gaze, a contact cone from the gaze spread, look-aways, and their timing
against speaking, listening and turns (:mod:`eye_contact.metrics`). See
:doc:`math/eye_contact`.

.. code-block:: python

   from eye_contact import metrics as ec

   yaw0, pitch0 = ec.find_mode(yaw[listening], pitch[listening])
   offsets = ec.angle_between(ec.to_direction(yaw, pitch), ec.to_direction(yaw0, pitch0))
   radius, sigma, share = ec.contact_radius(offsets[listening])
   state = ec.contact_states(offsets, radius, times_s)    # 1, 0 or nan per frame
   looks = ec.aversions(state, times_s, yaw, pitch, yaw0, pitch0)

.. automodule:: eye_contact
   :no-members:

.. autosummary::
   :nosignatures:

   gaze.GazeModel
   gaze.gaze_angles
   metrics.find_mode
   metrics.contact_radius
   metrics.contact_states
   metrics.aversions
   metrics.turn_patterns
   metrics.summarise

.. autoclass:: eye_contact.gaze.GazeModel
   :members: gaze

.. autofunction:: eye_contact.gaze.gaze_angles

.. autofunction:: eye_contact.metrics.find_mode

.. autofunction:: eye_contact.metrics.contact_radius

.. autofunction:: eye_contact.metrics.contact_states

.. autofunction:: eye_contact.metrics.aversions

.. autofunction:: eye_contact.metrics.turn_patterns

.. autofunction:: eye_contact.metrics.summarise

.. _analysis-api-conversation:

Conversation Timing
-------------------

Times a conversation from the session's audio and one camera's face. Speech
comes from Silero VAD (:mod:`conversation.vad`), audio is placed on the video
clock from the recorder's timing file (:mod:`conversation.timing`), speech
is attributed to the face on camera or someone else from mouth movement and
diarization labels (:mod:`conversation.speakers`), and turns, floor
transfer offsets, pauses, overlaps and backchannels follow
(:mod:`conversation.turns`). See :doc:`math/conversation_timing`.

.. code-block:: python

   from conversation import turns as tt

   subject = tt.merge_close(tt.mask_intervals(subject_mask, 0.01), tt.MIN_PAUSE_S)
   other = tt.merge_close(tt.mask_intervals(other_mask, 0.01), tt.MIN_PAUSE_S)
   spurts = tt.classify_spurts(subject, other)
   turns, transitions = tt.build_turns(spurts)
   [x.fto_s for x in transitions]   # response times, seconds

.. automodule:: conversation
   :no-members:

.. autosummary::
   :nosignatures:

   vad.detect_speech_silero
   vad.band_energy_db
   timing.fit_timing
   timing.estimate_av_lag
   speakers.mouth_activity
   speakers.speaking_threshold
   speakers.match_diarized_speaker
   speakers.attribute
   turns.classify_spurts
   turns.build_turns
   turns.summarise

.. autofunction:: conversation.vad.detect_speech_silero

.. autofunction:: conversation.vad.band_energy_db

.. autoclass:: conversation.timing.AudioClock
   :members:

.. autofunction:: conversation.timing.fit_timing

.. autofunction:: conversation.timing.estimate_av_lag

.. autofunction:: conversation.speakers.mouth_activity

.. autofunction:: conversation.speakers.speaking_threshold

.. autofunction:: conversation.speakers.match_diarized_speaker

.. autofunction:: conversation.speakers.attribute

.. autofunction:: conversation.turns.classify_spurts

.. autofunction:: conversation.turns.build_turns

.. autofunction:: conversation.turns.summarise

.. _analysis-api-report:

Session report
--------------

``run_session_report.py`` reads what the analysis plugins wrote for a session
(:mod:`report.collect`; any of them may be missing), writes one self-contained
HTML page (:mod:`report.render`: inline CSS and SVG, no scripts, light and
dark themes, printable), and the session's numbers as one flat row
(:mod:`report.summary`). ``--sessions-root`` does this for every session
below a folder and combines the rows into ``sessions_summary.csv``.

.. code-block:: python

   from report.collect import collect
   from report.render import render
   from report.summary import session_row, write_rows

   data = collect(session_path)
   html = render(data)
   write_rows("summary.csv", [session_row(data)])

.. automodule:: report
   :no-members:

.. autofunction:: report.collect.collect

.. autofunction:: report.summary.session_row

.. autofunction:: report.summary.write_rows

.. autofunction:: report.render.render

.. _analysis-api-motion:

Motion Tracking
--------------------

Run from the Session Browser, not a live Analysis-tab plugin (see
:doc:`user_guide`). Background-subtraction blob detection
(:class:`~motion.CentroidTracker`) plus greedy nearest-centroid tracking
across frames, independent of the Pose plugin's keypoint-based approach;
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

Not a post-hoc Analysis-tab plugin: this package backs the **Real-time**
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
incrementally. A segment is confirmed (final, never revised again) once
its end lies at least ``TRAILING_MARGIN_SEC`` before the buffer's current
end, giving it a margin of trailing audio context on both the previous
pass and this one; everything after that point is "tentative" text,
replaced wholesale each pass. Confirmed audio is then trimmed off the
buffer's front so growth stays bounded. This intentionally skips more
elaborate cross-pass textual-agreement ("LocalAgreement-n") policies some
streaming-ASR projects use. VAD-anchored segment boundaries are already
stable in practice for the confirmed prefix, and the simpler rule is
sufficient for a ``tiny``-model live-captions v1.
