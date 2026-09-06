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
3. *(Optional)* Fill in **Subject**, **Session** and **Task** above the
   Record button. These name the session folder in BIDS style — e.g.
   ``sub-P01_ses-pre_task-rest_run-01_20260906T143012`` — and the preview
   line under the fields shows exactly what will be created, run number
   included. Labels may contain letters and digits only; anything else is
   dropped, and the warning line says so before you commit.

   All three are optional. Left blank, the folder keeps the plain timestamp
   name, so you are never blocked from recording. The values carry over to
   the next session, so running one participant through several tasks means
   changing one field.

   If the combination already has recordings, MOSAIC asks before starting
   and offers the next ``run-`` number. Nothing is ever overwritten either
   way — the prompt only decides how the new session is numbered.
4. *(Optional)* Type anything worth remembering into the **Notes** box. It
   stays editable *during* the recording, and again afterwards from the
   Session Health dialog that appears on Stop — which is usually when you
   actually know what to write. Notes are saved as a plain ``notes.txt``
   beside the recording and are searchable from the Session Browser.
5. Click **● Record** (or press ``Ctrl+R``). After the start countdown the
   session folder is created and the elapsed-time display starts counting.
6. Trigger events (keyboard bindings, serial bytes, parallel-port edges —
   e.g. an EEG amplifier's trigger-out cable) are logged automatically to
   that session's ``trigger.csv`` for the whole duration.
7. Click **■ Stop** (or ``Ctrl+.``) to end the session.

See :doc:`recording` for exactly what gets written (``session_meta.json``,
per-camera timestamp CSVs, the post-hoc ``sync_manifest.json``) and why
cameras can end up with slightly different frame counts.

The Real-time tab — live dashboard
-----------------------------------------

Sitting between **Live** and **Analysis**, the **Real-time** tab is a
live-monitoring dashboard, not a post-hoc plugin — everything on it updates
continuously while cameras are open, whether or not a recording is
currently in progress.

**Per-camera tiles.** One tile per configured camera, each showing a live
thumbnail with a real-time pose skeleton and gaze-direction overlay drawn
on top (reusing the exact same live MediaPipe pipeline the Live tab's own
overlay toggles use — the Analysis tab's post-hoc Pose/Gaze results are a
*different*, independently-run, full-fidelity computation over recorded
video, not the same data). Each tile shows a pose-detection-quality badge,
a gaze-on-target percentage, and a small bucketed sparkline of recent
detection rate. An **Analyze** checkbox per tile lets you opt a specific
camera out of live analysis without affecting the others — useful if one
camera's feed doesn't need live tracking and you'd rather spend that
camera's share of the shared inference budget elsewhere.

**Auto-pause during recording.** Live pose/gaze analysis automatically
pauses the instant a recording starts, and resumes automatically when it
stops — a visible amber banner ("⏸ Analysis paused — recording in
progress") makes this state unambiguous. This is a deliberate resource
trade-off: recording's own video/audio pipeline gets full priority, and
every tile's last-known overlay stays visible (frozen, at reduced opacity)
rather than going blank, so you're never looking at a dead panel. Raw
camera thumbnails and the audio waveform keep updating throughout — only
the heavier pose/gaze inference and live transcription pause.

**Live audio waveform.** A shared, multi-channel bipolar waveform strip
along the bottom shows every configured microphone's live signal — the
same widget and scale control as the Live tab's own audio monitor.

**Live transcript.** A scrolling captions panel shows near-real-time
speech-to-text for microphone 1 (a ``tiny`` Whisper model, tuned for low
latency over accuracy) — confirmed text scrolls up the history; an italic
in-progress line shows the current, still-revising guess underneath it.
Like pose/gaze analysis, this pauses automatically during recording. If
the transcription subprocess isn't available (missing Python environment),
the panel shows a clear "unavailable" message instead of silently staying
empty.

.. note::

   The Real-time tab's live pose/gaze/transcript signals are **not saved
   anywhere** — they're a monitoring convenience, not a recorded artifact.
   For a persisted, full-fidelity result you can review and export later,
   run the corresponding Analysis-tab plugin (**Pose**, **Multi-Camera
   Gaze Fusion**) against the recorded video, or **Speaker Diarization**
   for a saved transcript.

Using the Session Browser
-------------------------------

The Session Browser (accessible from the main window) lists every recorded
session under the profile's configured recording directory, newest first.
Ordering is by each session's recorded start time (from ``session_meta.json``),
not by folder name — so it stays chronological regardless of how sessions are
named. The search box matches the folder name, the recording profile and the
session's notes.
For a selected session you can:

- **Play it back** — all cameras' videos in sync, aligned via the session's
  ``sync_manifest.json`` (generated on first playback if it doesn't exist
  yet).
- **Annotate it** — add timestamped labels during playback, exported as CSV.
- **Run Motion tracking** — the one plugin that only runs from here, not the
  Analysis tab (see :ref:`the note below <motion-tracking-note>`).
- See at a glance, via colored badges, which analyses have already been run
  on that session: **POSE**, **MOTION**, **TRANSCRIPT** (Speaker
  Diarization), **EXPRESSION**, **GAZE**, **3D POSE**, and **HR**
  (Remote Heart Rate). Face Masking has no badge of its own — check for
  the session's ``anonymized/`` folder directly, or open the Face Masking
  plugin on the Analysis tab, which reports whether it's already been run.

.. note::

   **Who sees which sessions.** A regular profile only ever sees its own
   recordings (its ``recordings/<username>/`` folder — see
   :doc:`recording`). An **admin** profile's Session Browser and Analysis
   tab instead show an *aggregated* view across every known profile's
   sessions (plus an ``_unassigned`` bucket for anything that couldn't be
   matched to a profile), each labeled with a ``@username`` badge — see
   :doc:`profiles` for the full recording-access-control model. This is a
   real filesystem-level separation, not just a UI filter: a non-admin
   profile's own Record Settings directory field is read-only, precisely
   so it can't be pointed at another profile's folder.

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
      one camera at a time, writing per-frame keypoints to a
      ``<session>/pose/video_N.pose.json`` file per camera (see
      :doc:`recording` for the full session layout).

      **Controls**: a model-size dropdown (shared with the Performance
      tab's own model picker) and a **skip** spinbox — analyzes every Nth
      frame instead of every frame. Skipped frames get *no* pose data at
      all (not interpolated); the overlay and chart simply fall back to
      the nearest analyzed frame for them. Higher values trade temporal
      resolution for speed on long recordings — keep it at 1 (the default)
      for the most complete result.

      **While it's running**: two progress bars track the run — a blue
      "Camera N/M" bar for overall session progress across cameras, and a
      green per-frame ``%`` bar underneath it for progress within the
      camera currently being processed.

      **Reading the output**: pick a camera, then a keypoint, in the
      results row. The video plays with a skeleton overlay — small dots at
      each detected landmark (nose, eyes, shoulders, elbows, wrists, hips,
      knees, ankles — 17 COCO keypoints), connected by lines into a
      skeleton — synced to playback. If a camera genuinely has no detected
      person anywhere in its footage (a framing/angle issue, not a bug), a
      status message says so explicitly instead of silently showing an
      empty video and chart.

      The chart alongside the video plots the selected keypoint's X/Y
      pixel position against elapsed time (seconds), with a chart title
      naming what's plotted, a legend distinguishing the two lines, and a
      hover tooltip on the curve for reading off an exact value. Clicking
      anywhere on the chart seeks the video to that point in time, and a
      dashed playhead line tracks the video's current position as it
      plays.

      **Speed / Acceleration**: switch the **Metric** dropdown from
      Position to Speed or Acceleration to see derived kinematics for the
      selected keypoint — see :doc:`math/pose_kinematics` for the math and
      the **Smoothing**/**Scale (mm/px)** controls below. Distance/average-
      speed/max-speed stats and a CSV export are available alongside the
      chart.

      **Multiple subjects**: when a session has more than one detected
      person, a row of colored **Subject** chips appears above the chart —
      check as many as you want plotted simultaneously (Position mode
      shows a solid/dashed X/Y pair per subject; Speed/Acceleration shows
      one line per subject), and the video overlay colors each detected
      person's skeleton to match. A single-subject session shows no chip
      row at all. Each **Subject** is one *tracked* person, followed across
      frames by BoT-SORT, so "Subject 1" stays the same physical person even
      when the detector reorders its output — the skeleton overlay labels
      each person so you can watch this directly. Three limits remain:
      someone who leaves the frame for long enough returns as a *new*
      Subject (each stats line shows the time span it actually covers, so a
      fragment is not mistaken for a whole recording); subject ids are
      per-video, so they are never comparable between cameras or between
      re-runs; and a chip marked *(untracked)*, drawn with a dashed border,
      is a detection the tracker never claimed and is still raw per-frame
      order. Results analysed before tracking existed keep the old
      detection-order behaviour and say so in their chip tooltips.

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
      Start/End/Speaker/Text rows, each attributed row marked with a 🗣
      glyph, colored and lightly tinted per speaker, and clicking a row
      seeks audio playback. The active row highlights automatically during
      playback. The waveform above the table is shaded with a matching
      color band (a background wash plus a crisp top/bottom edge strip
      carrying the speaker's name where the turn is wide enough to hold it)
      for every diarized speaker turn, with a color-swatch legend
      underneath — clicking anywhere on the waveform also seeks playback.
      Time ticks along the middle of the waveform give each turn a
      readable position. A stretch of waveform with no color band means no
      speaker was confidently attributed there, not a rendering gap. See
      :doc:`math/speaker_diarization` for the max-overlap
      speaker-assignment rule.

      .. important::

         Diarization needs a **free Hugging Face token** with the terms of
         use accepted for ``pyannote/speaker-diarization-community-1``
         (generate a token at
         `huggingface.co/settings/tokens
         <https://huggingface.co/settings/tokens>`_).

         **Both steps are required.** A token whose owner has not accepted
         that model's terms fails at load with a 401 that looks exactly
         like a bad token, and is the most common reason a correct-looking
         setup still produces no speakers.

         MOSAIC will not start a run that cannot produce speaker labels: if
         the token field is empty and "Transcript only" is unticked, Run
         stops immediately rather than spending minutes on transcription to
         reach an unlabelled result. Tick "Transcript only" to transcribe
         without speakers deliberately.

      **If the Speaker column is blank**, a banner above the waveform says
      why — no token, terms not accepted, the model failing to load, or the
      model running and finding no speaker turns — together with what to do
      about it. That reason is recorded in the transcript file itself
      (``diarization_status``), so it is still available long after the run
      log has scrolled away. Transcripts produced by older versions have no
      such record; those report only that labels are missing.

   .. tab-item:: Facial Expression

      **What it does**: detects faces (MediaPipe FaceLandmarker) and
      classifies each into a dominant emotion, per frame, writing one
      ``<session>/expression/video_N.expression.json`` file per camera (see
      :doc:`recording` for the full session layout).

      **Controls**:

      - **Backend** — **Heuristic** (default): a transparent, weighted
        blendshape-scoring rule table with zero extra download or model —
        see :doc:`math/facial_expression` for the exact formula. **FER+**: a
        pretrained 8-class ONNX CNN (downloads an extra ~34MB model on first
        use), generally more accurate and the only backend that can report
        "Contempt," at the cost of being a less transparent black box than
        the heuristic. **py-feat**: the most detailed of the three — real
        FACS Action Units (20 individually-scored muscle movements, e.g.
        AU12 = lip corner puller), not just a single emotion label —
        meaningfully slower (~0.1–0.8s/frame on CPU) and pulls in the
        heaviest dependency (torch + torchcodec, needing a compatible
        FFmpeg discoverable on ``PATH``); pick it when the research
        question is about *which muscles moved*, not just an overall
        emotion. Blendshape scores themselves are always computed and
        stored regardless of which backend is selected — only the
        *dominant-expression* label/score (and, for py-feat, the AU values)
        differs.
      - **Max faces** (default 5) — the most faces detected simultaneously
        in one frame. Raise it for a session with more people in frame at
        once; extra faces beyond this cap are simply not detected, they
        don't error out.
      - **Min confidence** (default 0.5) — the detection/presence threshold
        a candidate face must clear to count as detected. Lower it if real
        faces at odd angles or partial occlusion are being missed; raise it
        if the detector is picking up false positives. This is unrelated to
        the *classification* confidence shown per detected face — it only
        gates whether a face is detected at all.
      - **Skip** (default 1 = every frame) — analyzes every Nth frame
        instead of every frame, the same trade-off as Pose's skip control:
        skipped frames get no expression data at all (not interpolated),
        and both the video overlay and the chart fall back to the nearest
        analyzed frame for them. Raising it speeds up long recordings at
        the cost of missing brief expression changes between analyzed
        frames — keep it at 1 for the most complete result.

      **Reading the output**: the selected camera's video plays with a
      bounding box + dominant-expression label per detected face. A
      **Blendshape** dropdown drives a chart of that blendshape's score over
      time — the chart's time axis always spans the full analyzed range of
      the video, even for stretches where no face was detected (those
      simply show a gap in the plotted line, not a shortened axis) — and a
      stats readout shows the %-breakdown across expression categories for
      the run. See :doc:`math/facial_expression` for the scoring/softmax
      formulas. Like Pose kinematics, subject identity isn't tracked
      frame-to-frame.

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

   .. tab-item:: 3D Pose Reconstruction

      **What it does**: triangulates every camera's already-computed 2D
      keypoints (the **Pose** plugin's own output — run Pose on at least
      one camera first, or this plugin refuses to run with a clear error)
      into full 3D skeletons, one per detected person, per frame, with a
      stable identity across frames. Also needs
      :ref:`room (extrinsic) calibration <room-extrinsic-calibration>`
      completed first, exactly like Gaze Fusion.

      **Controls**: minimum-cameras-to-triangulate (2–6, default 2),
      maximum reprojection error in pixels (default 15 — a keypoint whose
      3D triangulation reprojects further than this from any contributing
      camera's real observation is dropped rather than trusted), and a
      frame-skip spinbox.

      **Reading the output**: an interactive 3D room view — drag to orbit,
      scroll to zoom, double-click to reset — shows a floor grid, every
      calibrated camera's position, and every currently-tracked person's
      3D skeleton, color-coded by track ID. A **Track** dropdown filters
      the stats/CSV export to one track or "All." The selected camera's
      video also shows the reprojected 2D skeleton overlay, letting you
      visually confirm the 3D reconstruction actually lines up with what
      that camera really saw. See :doc:`math/pose3d_reconstruction` for the
      triangulation, cross-camera association, and tracking math — and
      its important caveat about 2-camera rigs with multiple people.

   .. tab-item:: Remote Heart Rate (rPPG)

      .. important::

         **EXPERIMENTAL.** This is a research-grade heart-rate estimate
         only — not a medical device, not clinically validated. A
         persistent banner in the results panel says so for exactly this
         reason. See :doc:`math/remote_heart_rate` before trusting any
         output number, and cross-check against a real reference (pulse
         oximeter, manual pulse count) if accuracy actually matters.

      **What it does**: estimates heart rate from subtle, camera-visible
      color changes in facial skin (remote photoplethysmography) — no
      contact sensor needed, but correspondingly less reliable than one.
      Detects a face-skin region per frame, combines its color signal into
      a pulse waveform, and extracts a BPM estimate per sliding time
      window.

      **Controls**: a **Backend** dropdown — **Green** (fast naive
      baseline, no motion compensation), **CHROM** (chrominance-based),
      **POS** (default — generally the most robust classical method) —
      plus **Window** and **Hop** length in seconds (defaults 10s/2s) and
      a smoothing-windows spinbox. There is deliberately no frame-skip
      control here, unlike every other plugin: skipping frames would
      undersample the pulse signal itself below what's needed to resolve a
      heart rate at all.

      **Reading the output**: a BPM-over-time chart (toggle raw vs.
      smoothed), a stats readout (mean/median/min/max BPM, % of windows
      that produced a reliable estimate), and a quality badge combining
      signal-to-noise ratio and face-detection density into one
      Excellent/Good/Acceptable/Poor tier — treat "Poor" as "don't trust
      this number." A debug overlay on the video shows the tracked
      face-skin ROI box plus a live BPM readout, so you can visually
      confirm the algorithm is tracking real skin, not hair or background.
      A window with insufficient face detection reports no BPM at all
      rather than a guessed one — this is expected on footage where the
      subject doesn't hold still facing the camera, not a bug.

   .. tab-item:: EEG/Trigger ↔ Frame Sync

      **What it does**: resolves every logged trigger event (keyboard,
      serial, parallel-port — e.g. an EEG amplifier's trigger-out cable)
      to its nearest captured frame in every camera, so you know exactly
      what each camera recorded at the instant of each external event.
      Unlike every other plugin on this page, this one runs **synchronously
      in the app itself** — no subprocess, no progress bar, just a
      near-instant table once you click Run (or automatically, if it's
      already been run for this session).

      **Reading the output**: one row per trigger event (elapsed time,
      wall clock, source, label, value) plus a resolved frame number and
      timing offset (``Δms``) for every camera. Click a row to seek the
      currently-selected camera's video to that frame. Export as CSV for
      use in external EEG-analysis tooling (e.g. MNE-Python). See
      :doc:`recording`'s "Aligning streams in Python" section for the
      manual/scripted equivalent, and why ``trigger.csv``'s
      ``elapsed_ns`` column — not ``elapsed_ms`` — is the one safe to
      compare across files.

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
      flag needed. FER+, face-mask detectors, py-feat, rPPG, and 3D Pose
      Reconstruction all run CPU-only by design — the models are small
      enough (FER+, face-mask), the underlying library doesn't auto-select
      a device (py-feat), or there's simply no GPU-acceleratable step at
      all (rPPG's classical signal processing; 3D Pose Reconstruction's
      pure linear algebra) — for all of these, a GPU wouldn't meaningfully
      help.

   .. grid-item-card:: 🧑‍🤝‍🧑  Subject identity across frames

      **Pose** tracks identity across frames (BoT-SORT), and **3D Pose
      Reconstruction** does its own nearest-centroid matching per tick — but
      neither is a guarantee: a long occlusion ends a track, and the person
      returns under a new id. The two numbering systems are also unrelated,
      so Pose's "Subject 2" and the room view's "track 2" are not the same
      label. Ids are per-video and per-run: never comparable between
      cameras, or between two analyses of the same footage.

      **Facial Expression** and **2D Gaze** still do *not* track identity —
      their "subject 0" means "first detection in that frame" — so their
      stats remain fully reliable only for single-subject sessions.

   .. grid-item-card:: ❤️  rPPG is experimental — verify before trusting

      Remote Heart Rate estimates are research-grade only, not clinically
      validated. They also need a subject holding reasonably still and
      facing a camera for the whole analysis window (10s by default) —
      footage where nobody looks steadily at a camera will correctly
      report no reliable estimate rather than a fabricated number. See
      :doc:`math/remote_heart_rate` for the full accuracy discussion.

   .. grid-item-card:: 🎬  py-feat's FFmpeg requirement

      The py-feat Facial Expression backend needs a torchcodec-compatible
      FFmpeg (versions 4–8, a shared/DLL build) discoverable on ``PATH`` —
      even though it never touches video I/O directly. If this backend
      fails to construct, check ``where ffmpeg`` before suspecting a code
      issue.

   .. grid-item-card:: 📁  Where recordings actually live

      Each profile's sessions live under its own
      ``recordings/<username>/`` folder, resolved relative to wherever the
      app is running from — not a single shared folder. Only an admin
      profile sees every profile's sessions at once (see the note above);
      a regular profile's Record Settings directory field is read-only for
      exactly this reason.
