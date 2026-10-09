User guide
==========

.. contents:: On this page
   :local:
   :depth: 2

This page walks through MOSAIC's day-to-day workflow: preparing the rig,
recording a session, checking what was recorded, and running the analysis
plugins. For what is written to disk, file by file, see :doc:`recording`;
for every camera control, see :doc:`camera_settings`.

.. grid:: 1 2 3 3
   :gutter: 2

   .. grid-item-card:: 🎬  Record a session
      :link: recording-a-session
      :link-type: ref

      From an idle rig to a finished, named recording, with the checks
      MOSAIC runs for you on the way.

   .. grid-item-card:: 🎯  Room or interview
      :link: room-and-interview-mode
      :link-type: ref

      Every camera at up to 25 fps, or one camera cropped and faster.

   .. grid-item-card:: 🎞️  Equal frame counts
      :link: synced-videos
      :link-type: ref

      ``synced/``: every camera the same length, one frame per trigger,
      with missed frames marked.

   .. grid-item-card:: 🩺  When something goes wrong
      :link: acquisition-troubleshooting
      :link-type: ref

      A camera that falls behind, drops out, or runs slower than asked.

   .. grid-item-card:: 🗂️  Browse and play back
      :link: using-the-session-browser
      :link-type: ref

      Find a session, play every camera in sync, see what was analysed.

   .. grid-item-card:: 🔬  Analyse
      :link: running-an-analysis-tab-plugin
      :link-type: ref

      Pose, gaze, expression, heart rate, transcripts and more.

.. _recording-a-session:

Recording a session
-------------------

.. rubric:: 1. Open the rig

Launch MOSAIC and log in to (or create) your research group's profile; see
:doc:`profiles`. The cameras open on their own and the **Live** tab shows
one tile per camera.

.. rubric:: 2. Check the cameras

Look along the header row above the live feeds. Beside the camera count,
each camera has a small chip with its measured frame rate, for example
``Cam 3  24.9``.

- A **neutral** chip means the camera is keeping up with the others.
- **Amber** means it delivers less than 70 % of what the other cameras manage.
- **Red** means less than 40 %, or that it has stopped delivering altogether
  (no new frame for 2 s, which is what an unplugged cable looks like).
- **No chip** means the camera is not grabbing at all: closed, not broken.

Hover a chip for the explanation. A camera far behind its neighbours almost
always has a cable or network-port problem; fix it now, because nothing can
recover those frames afterwards.

.. note::

   The chips compare each camera with the *other* cameras, not with the
   frame rate you configured. If every camera runs below the configured rate
   together, that is a setting to change (see
   :ref:`frame-rate-lower-than-asked`), not a failing camera, and the chips
   stay calm.

.. rubric:: 3. Name the recording

Fill in **Subject** above the Record button, and optionally **Session** and
**Task**. They name the session folder in BIDS style, for example
``sub-P01_ses-pre_task-rest_run-01_20260906T143012``, and the line under the
fields shows exactly what will be created, run number included. Labels keep
letters and digits only; anything else is dropped, and the warning line says
so before you record.

The values carry over to the next session, so running one participant
through several tasks means changing one field. Use **Clear** between
participants. A combination that has been recorded before simply gets the
next ``run-`` number: nothing is ever overwritten.

.. rubric:: 4. Press Record

Click **● Record** or press ``Ctrl+R``. Before anything starts, MOSAIC checks
the rig as it is at that moment (the **pre-flight check**):

.. list-table::
   :header-rows: 1
   :widths: 22 39 39

   * - Check
     - 🔴 Red: data would be lost
     - 🟠 Amber: likely a problem
   * - Cameras
     - Configured but not open; open but no frames arriving; stopped
       delivering (no frame for 3 s)
     - Below 90 % of its own frame rate; set to hardware trigger but
       running unsynchronised
   * - Microphones
     -
     - None configured; a configured microphone not connected (the system
       default input would be recorded instead)
   * - Disk
     - Less than 10 minutes of recording left
     - Less than 60 minutes left, or the free space cannot be read

**If everything passes, nothing appears:** the countdown starts as usual.
Otherwise the *Name this recording* dialog opens, titled *Before recording*,
with the problems first and the checks that passed below them, grouped (for
example "5 cameras ready"). Fix the problem and click Record again, or click
**Record anyway**: nothing is ever blocked. Whatever you recorded despite is
written into the session's ``session_meta.json`` as ``preflight_warnings``,
so the analysis later knows.

The same dialog also asks for a subject when none is filled in. Recordings
started by an external trigger are never checked or interrupted by a dialog:
their timing belongs to the experiment. The check can be turned off under
**Settings → Record → Starting a Recording**.

.. rubric:: 5. While recording

- With **hide previews while recording** on (the default), the camera grid
  is replaced by a single row of chips showing each camera's real frame rate
  in the same colours, so the subject is not distracted and you still see
  every camera.
- A camera that falls behind or stops is written once to the log
  (``[Health] Camera 3 is delivering far fewer frames than the others``), and
  once more when it recovers.
- Type anything worth remembering into the **Notes** box. It stays editable
  during the recording and is saved as ``notes.txt`` beside it.
- Trigger events (keyboard bindings, serial bytes, parallel-port edges such
  as an EEG amplifier's trigger cable) are logged to the session's
  ``trigger.csv``.

.. rubric:: 6. Stop

Click **■ Stop** or press ``Ctrl+.``. The **Session Health** report opens:
one row per camera with frames captured, dropped and incomplete, and the
synchronisation quality. Shortly afterwards, **Frame Sync Repair** starts in
the background and writes the equal-length videos into ``synced/`` (see
:ref:`synced-videos`). It never starts while a new recording is running; it
waits until that one stops.

.. _room-and-interview-mode:

Room mode and interview mode
----------------------------

The **Room | Interview** switch above the live feeds chooses between two ways
of recording. Switching closes and reopens the cameras, which takes a few
seconds; it is not possible during a recording.

.. tab-set::

   .. tab-item:: Room mode

      Every configured camera records the full 1920 × 1080 image, triggered
      together by GigE Action Commands so their frames line up.

      - Each camera needs 38.3 ms to read out a full frame, so a camera can
        deliver up to about **26 fps**; the room is set to 25.
      - Triggered cameras are paced together at 85 % of the slowest
        camera's rate. With all cameras set to 25 fps, the room records at
        about **21 fps**.
      - Each camera's achievable rate is shown on its card in
        **Settings → Video**.

   .. tab-item:: Interview mode

      One camera records alone (by default **Camera 3**, facing the
      subject), cropped and faster. The other cameras are closed.

      Set it up under **Settings → Video → Interview mode** (click the
      header to open the section): camera, crop, frame rate and exposure
      limit. Edits take effect when you click **Apply**.

      **The crop's height sets the frame rate.** The sensor is read row by
      row, so fewer rows mean more frames; width and exposure hardly matter.
      Measured on Camera 3:

      .. list-table::
         :header-rows: 1
         :widths: 50 50

         * - Crop
           - Maximum
         * - 1920 × 1080 (full frame)
           - about 26 fps
         * - 1280 × 720
           - 38.9 fps
         * - 1280 × 540
           - 51.6 fps

      As soon as a crop is applied, the section shows the camera's own
      maximum for it and offers **Use N fps** when you asked for more. The
      badge above the live view shows the rate the camera really delivers,
      for example ``Cam 3 only · 38.9 fps (asked 50)`` in amber when it is
      short.

      Only that camera's video is written (``video/video_2.mp4`` for Camera
      3), and ``session_meta.json`` records ``"mode": "interview"`` with the
      requested and the camera-reported frame rate.

.. tip::

   **A lower exposure limit may not make the image darker.** With automatic
   gain, the camera raises its gain to keep the same brightness, so the image
   gets noisier rather than darker. The limit still applies; the log shows
   what the camera settled on (``Auto settled: exposure … us, gain …``).

.. _synced-videos:

Equal frame counts: ``synced/``
-------------------------------

The videos in ``video/`` are kept exactly as recorded, and they rarely have
the same length: a camera can join a few triggers late, lose frames, or stop a
little early. After each recording, MOSAIC writes an aligned copy of every
camera's video into ``synced/``:

- **Every camera has the same number of frames**, one per trigger, over the
  time every camera was recording.
- **Frame *k* is the same moment in every camera**: it is the frame that
  trigger *k* produced. MOSAIC logs every trigger it sends
  (``video/action_ticks.csv``) and matches each frame to its trigger using
  the camera's own clock.
- **A frame a camera missed is visible.** It shows that camera's last real
  frame with a small red **MISSING** tag in the top-left corner, so motion
  stays continuous and the gap cannot be mistaken for a real image.
- **A camera that drops out does not shorten the others.** Small start and
  stop differences (under 2 s) are trimmed so all cameras start and end
  together. A camera unplugged mid-session is shown as MISSING for the time
  it was absent, and the other cameras keep their full length.

The Analysis tab's **Frame Sync Repair** page lists, per camera, the frames
missing (hover for the exact ranges), the frames trimmed, and how it was
aligned. Older sessions, and sessions without hardware triggering, are aligned
on arrival time instead; the page says which.

The automatic run can be turned off under **Settings → Record**, and it is
skipped for single-camera sessions (interview mode), where there is nothing to
line up.

If MOSAIC crashes
-----------------

A crash, a forced close or a power cut no longer costs the whole session.
Videos are written in self-contained pieces of about 2 seconds, and the
timestamp, audio and trigger files reach the disk every second, so at most
the last couple of seconds are lost.

A session that never finished is tagged **INTERRUPTED** in the Session
Browser and the Analysis tab; it can be played and analysed like any other.
A session being recorded right now, on this machine or another one sharing
the recordings folder, is tagged **RECORDING** instead. See
:doc:`recording` for the details.

.. _acquisition-troubleshooting:

Troubleshooting acquisition
---------------------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - What you see
     - What to do
   * - A camera's chip is amber or red
     - Reseat its cable at both ends, or move it to another network port.
       If it stays behind while the others keep up, it is that camera's link.
   * - "Camera N is not open" in the pre-flight dialog
     - Check its cable and power, then reopen the cameras from
       **Settings → Video**. An unplugged camera only comes back when the
       cameras are reopened.
   * - A camera's ``synced/`` video shows MISSING frames
     - Those triggers produced no frame from that camera. A few scattered
       ones point to its link; a long block means it dropped out.
   * - The log says "missing trigger broadcasts" or "packet loss"
     - Read the end of the line: *packet loss* means frames were damaged in
       transit (cable, port); *missing trigger broadcasts* means they never
       came (trigger path).
   * - The recording's frame rate is lower than configured
     - See :ref:`frame-rate-lower-than-asked`.

.. _frame-rate-lower-than-asked:

.. rubric:: Frame rate lower than asked

Each camera's card in **Settings → Video** shows the rate the camera itself
says it can deliver. If it is below what you configured:

- **Room mode:** a full frame takes 38.3 ms to read out, so about 26 fps is
  the ceiling, and triggered recording runs at 85 % of the slowest camera.
- **Interview mode:** a shorter crop (fewer rows) raises the rate; width and
  exposure barely change it. Use the **Use N fps** button to ask for exactly
  what the camera can do.
- **Exposure:** an exposure limit longer than one frame (more than 40 ms at
  25 fps) can hold the rate down in dim light. The interview section warns
  when that is the case.

The Real-time tab: live dashboard
---------------------------------

Sitting between **Live** and **Analysis**, the **Real-time** tab is a live
monitoring dashboard, not an analysis plugin: it updates continuously while
the cameras are open, whether or not a recording is running.

**Per-camera tiles.** One tile per camera, each with a live thumbnail, a
real-time pose skeleton and gaze-direction overlay, a pose-detection quality
badge, a gaze-on-target percentage and a small sparkline of recent detection
rate. An **Analyze** checkbox per tile takes one camera out of live analysis
without affecting the others, leaving more of the shared processing budget
for the rest.

**Paused while recording.** Live pose, gaze and transcription pause the moment
a recording starts and resume when it stops, so the recording gets the
machine's full attention. An amber banner says so, and each tile keeps its
last overlay (dimmed) rather than going blank. Camera thumbnails and the audio
waveform keep updating.

**Live audio waveform.** A strip along the bottom shows every microphone's
live signal, with the same scale control as the Live tab's audio monitor.

**Live transcript.** A scrolling caption panel shows near-real-time speech to
text for microphone 1, using a small, fast Whisper model. Confirmed text
scrolls up; the line still being revised is shown in italics underneath. If
the transcription process is not available, the panel says so instead of
staying empty.

.. note::

   The Real-time tab's pose, gaze and transcript signals are **not saved**.
   They are for monitoring. For results you can review and export later, run
   the matching Analysis-tab plugin (**Pose**, **Multi-Camera Gaze
   Fusion**, or **Speaker Diarization** for a saved transcript) on the
   recorded video.

.. _using-the-session-browser:

Using the Session Browser
-------------------------

The Session Browser lists every session in your profile's recordings folder,
newest first, ordered by when each session was recorded (not by folder name).
The search box matches the folder name, the profile and the session's notes.
For a selected session you can:

- **Play it back:** all cameras in sync, aligned through the session's
  ``sync_manifest.json`` (created on first playback if needed).
- **Annotate it:** add timestamped labels during playback, exported as CSV.
- **Run Motion tracking:** the one plugin that runs only from here, not from
  the Analysis tab (see :ref:`the note below <motion-tracking-note>`).
- **See its state at a glance** from the coloured badges:

  - **INTERRUPTED** (red): the recording never finished; its files end where
    MOSAIC stopped.
  - **RECORDING** (green): it is being recorded right now.
  - **SYNCED**: the equal-length videos in ``synced/`` exist.
  - **POSE**, **MOTION**, **TRANSCRIPT** (Speaker Diarization),
    **EXPRESSION**, **GAZE**, **3D POSE**, **HR** (Remote Heart Rate),
    **GAZE 2D**, **FACE DYN**: that analysis has been run. Face Masking has no badge of its
    own; look for the session's ``anonymized/`` folder.

.. note::

   **Who sees which sessions.** A regular profile only sees its own
   recordings (its ``recordings/<username>/`` folder; see :doc:`recording`).
   An **admin** profile's Session Browser and Analysis tab show every
   profile's sessions together (plus an ``_unassigned`` group for anything
   that could not be matched to a profile), each labelled with a
   ``@username`` badge; see :doc:`profiles`. This is a real separation on
   disk, not only a display filter: a regular profile's recordings folder
   setting is read-only, so it cannot be pointed at another profile's folder.

.. _running-an-analysis-tab-plugin:

Running an Analysis-tab plugin
------------------------------------

Every plugin below (except Motion) lives on the top-level **Analysis** tab,
sitting alongside **Live**. The workflow is always the same shape:

1. Pick a session from the left-hand session list.
2. Pick a plugin from the plugin dropdown.
3. Set that plugin's controls (model/backend/etc.; see the tab below for
   each one).
4. Click **Run**. Progress streams into the log view; you can queue another
   session's run while one is in progress, since jobs serialize through one
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
      tab's own model picker) and a **skip** spinbox, which analyzes every
      Nth frame instead of every frame. Skipped frames get *no* pose data
      at all (not interpolated); the overlay and chart simply fall back to
      the nearest analyzed frame for them. Higher values trade temporal
      resolution for speed on long recordings; keep it at 1 (the default)
      for the most complete result.

      **While it's running**: two progress bars track the run: a blue
      "Camera N/M" bar for overall session progress across cameras, and a
      green per-frame ``%`` bar underneath it for progress within the
      camera currently being processed.

      **Reading the output**: pick a camera, then a keypoint, in the
      results row. The video plays with a skeleton overlay (small dots at
      each detected landmark, namely nose, eyes, shoulders, elbows, wrists,
      hips, knees and ankles for 17 COCO keypoints, connected by lines into
      a skeleton), synced to playback. If a camera genuinely has no
      detected person anywhere in its footage (a framing/angle issue, not
      a bug), a status message says so explicitly instead of silently
      showing an empty video and chart.

      The chart alongside the video plots the selected keypoint's X/Y
      pixel position against elapsed time (seconds), with a chart title
      naming what's plotted, a legend distinguishing the two lines, and a
      hover tooltip on the curve for reading off an exact value. Clicking
      anywhere on the chart seeks the video to that point in time, and a
      dashed playhead line tracks the video's current position as it
      plays.

      **Speed / Acceleration**: switch the **Metric** dropdown from
      Position to Speed or Acceleration to see derived kinematics for the
      selected keypoint. See :doc:`math/pose_kinematics` for the math and
      the **Smoothing**/**Scale (mm/px)** controls below. Distance/average-
      speed/max-speed stats and a CSV export are available alongside the
      chart.

      **Multiple subjects**: when a session has more than one detected
      person, a row of colored **Subject** chips appears above the chart.
      Check as many as you want plotted simultaneously (Position mode
      shows a solid/dashed X/Y pair per subject; Speed/Acceleration shows
      one line per subject), and the video overlay colors each detected
      person's skeleton to match. A single-subject session shows no chip
      row at all. Each **Subject** is one *tracked* person, followed across
      frames by BoT-SORT, so "Subject 1" stays the same physical person even
      when the detector reorders its output; the skeleton overlay labels
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
      (faces or whole people blurred or boxed) in a sibling ``anonymized/``
      folder. The originals in ``video/`` are never touched.

      **Controls**: a **Region** (**Face**, the default, blurs detected
      face boxes; **Whole body** blurs each person's whole silhouette,
      removing clothing and posture cues too), a detection backend
      (**MediaPipe**: default, best recall; **YOLOv8-face**: community
      checkpoint; **OpenCV DNN**: no extra ML framework, weaker on extreme
      angles), a style (**Blur** or **Solid box**), and a frame-skip
      spinbox (kept low: skipped frames reuse the last detected box rather
      than going unmasked, so raising it trades fidelity for speed, not
      privacy).

      **Whole body** masks the union of person segmentation and the face
      detector, so someone the segmenter misses still has their face
      covered. It is slower, it disables frame-skip (a reused silhouette
      misaligns as people move, where a padded face box does not), and it
      will also blur people appearing in mirrors or on screens in shot.

      Output files are named by region and backend
      (``video_0.body.mediapipe.mp4``), so runs with different coverage no
      longer overwrite each other; switching either control shows that
      variant's saved output.

      **Reading the output**: the selected camera's anonymized video plays
      directly in the results panel (no overlay/chart, since the mask is
      already baked into the video). An **Open output folder** button jumps
      straight to the ``anonymized/`` folder. Note the anonymized copy has
      no audio track. See :doc:`math/face_masking` for the padding,
      dilation and blur-kernel formulas.

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
      underneath. Clicking anywhere on the waveform also seeks playback.
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

      **Acoustics**: the **▶ Acoustics** button beside the mic picker runs a
      separate, fast pass (praat-parselmouth) that draws a spectrogram beneath
      the waveform, with the pitch track (cyan, logarithmic) and intensity
      contour (amber) over it and the same speaker colours marking each turn.
      It needs no Hugging Face token and does not re-transcribe, so a session
      that was never diarized can still be examined acoustically, and getting
      a spectrogram never costs another Whisper run. Results are cached beside
      the audio as ``<mic>.voice.json`` and ``<mic>.voice.png``, so re-opening
      the session is instant. **Pitch** and **Level** toggle the two overlays.

      The spectrogram shares the waveform's time axis exactly, so the playhead
      and every speaker turn line up between the two strips; clicking either
      seeks playback. It shows 0-5 kHz by default, which is Praat's own default
      view and keeps a low voice's harmonics from moiréing at this height.
      A gap in the pitch line is not missing data: it means the frame was
      unvoiced, and the line is deliberately broken rather than drawn across it.

      **If the Speaker column is blank**, a banner above the waveform says
      why (no token, terms not accepted, the model failing to load, or the
      model running and finding no speaker turns), together with what to do
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

      - **Backend**: **Heuristic** (default) is a transparent, weighted
        blendshape-scoring rule table with zero extra download or model;
        see :doc:`math/facial_expression` for the exact formula. **FER+**: a
        pretrained 8-class ONNX CNN (downloads an extra ~34MB model on first
        use), generally more accurate and the only backend that can report
        "Contempt," at the cost of being a less transparent black box than
        the heuristic. **py-feat**: the most detailed of the three, with real
        FACS Action Units (20 individually-scored muscle movements, e.g.
        AU12 = lip corner puller), not just a single emotion label. It is
        meaningfully slower (~0.1–0.8s/frame on CPU) and pulls in the
        heaviest dependency (torch + torchcodec, needing a compatible
        FFmpeg discoverable on ``PATH``); pick it when the research
        question is about *which muscles moved*, not just an overall
        emotion. Blendshape scores themselves are always computed and
        stored regardless of which backend is selected; only the
        *dominant-expression* label/score (and, for py-feat, the AU values)
        differs.
      - **Max faces** (default 5): the most faces detected simultaneously
        in one frame. Raise it for a session with more people in frame at
        once; extra faces beyond this cap are simply not detected, they
        don't error out.
      - **Min confidence** (default 0.5): the detection/presence threshold
        a candidate face must clear to count as detected. Lower it if real
        faces at odd angles or partial occlusion are being missed; raise it
        if the detector is picking up false positives. This is unrelated to
        the *classification* confidence shown per detected face; it only
        gates whether a face is detected at all.
      - **Skip** (default 1 = every frame): analyzes every Nth frame
        instead of every frame, the same trade-off as Pose's skip control:
        skipped frames get no expression data at all (not interpolated),
        and both the video overlay and the chart fall back to the nearest
        analyzed frame for them. Raising it speeds up long recordings at
        the cost of missing brief expression changes between analyzed
        frames, so keep it at 1 for the most complete result.

      **Reading the output**: the selected camera's video plays with a
      bounding box + dominant-expression label per detected face. A
      **Blendshape** dropdown drives a chart of that blendshape's score over
      time. The chart's time axis always spans the full analyzed range of
      the video, even for stretches where no face was detected (those
      simply show a gap in the plotted line, not a shortened axis). A
      stats readout shows the %-breakdown across expression categories for
      the run. See :doc:`math/facial_expression` for the scoring/softmax
      formulas. Like Pose kinematics, subject identity isn't tracked
      frame-to-frame.

   .. tab-item:: Multi-Camera Gaze Fusion

      **What it does**: finds where every person in the room looks, in 3D,
      using every calibrated camera that sees them: at whom (and whether
      they look at each other), at which named region (a screen, a toy), at
      the calibrated plane, or simply at which point. It needs each camera's
      intrinsic calibration, and
      :ref:`room (extrinsic) calibration <room-extrinsic-calibration>` to
      combine cameras. It works best on a session that has been through
      Frame Sync Repair (its ``synced/`` videos), and uses the raw videos
      otherwise.

      **Controls**:

      - **min cams**: cameras that must see a person before their gaze is
        reported (default 1).
      - **min conf**: face detector threshold.
      - **skip**: analyse every Nth frame (default 2; the videos still show
        every frame).
      - **subjects**: how many people to keep (auto keeps everyone seen for
        at least a second).

      **Reading the output**:

      - The camera video plays with every person's face box, name, gaze ray,
        gaze point, a short trail, and what they look at (``S1 -> S2``,
        ``S1 <-> S2`` when they look at each other). These annotated videos
        are rendered by the analysis into the session's ``gaze_fusion/``
        folder; **Open output folder** shows them, with a top-down room
        video and a heat map per person.
      - The room view beside it shows the room from above: cameras, named
        regions, every person and their gaze, and below it what each one
        looks at right now.
      - The stats line gives, per person, how much of the time a gaze was
        found and what they looked at most, plus the share of time with
        mutual gaze.
      - **Export CSV** writes one row per frame and person.

      To name people (``S1`` becomes ``Child``) or add regions after
      recording, put a ``gaze_targets.json`` in the session folder (see
      :ref:`analysis-api-gaze`) and run again: the cached face landmarks
      make that take seconds.

      See :doc:`math/gaze_fusion` for the method, its accuracy and its
      limits, and :doc:`math/room_calibration` for how camera positions are
      solved.

   .. tab-item:: 3D Pose Reconstruction

      **What it does**: triangulates every camera's already-computed 2D
      keypoints (the **Pose** plugin's own output; run Pose on at least
      one camera first, or this plugin refuses to run with a clear error)
      into full 3D skeletons, one per detected person, per frame, with a
      stable identity across frames. Also needs
      :ref:`room (extrinsic) calibration <room-extrinsic-calibration>`
      completed first, exactly like Gaze Fusion.

      **Controls**: minimum-cameras-to-triangulate (2–6, default 2),
      maximum reprojection error in pixels (default 15; a keypoint whose
      3D triangulation reprojects further than this from any contributing
      camera's real observation is dropped rather than trusted), and a
      frame-skip spinbox.

      **Reading the output**: an interactive 3D room view (drag to orbit,
      scroll to zoom, double-click to reset) shows a floor grid, every
      calibrated camera's position, and every currently-tracked person's
      3D skeleton, color-coded by track ID. A **Track** dropdown filters
      the stats/CSV export to one track or "All." The selected camera's
      video also shows the reprojected 2D skeleton overlay, letting you
      visually confirm the 3D reconstruction actually lines up with what
      that camera really saw. See :doc:`math/pose3d_reconstruction` for the
      triangulation, cross-camera association, and tracking math, and for
      its important caveat about 2-camera rigs with multiple people.

   .. tab-item:: Remote Heart Rate (rPPG)

      .. important::

         **EXPERIMENTAL.** This is a research-grade heart-rate estimate
         only: not a medical device, not clinically validated. A
         persistent banner in the results panel says so for exactly this
         reason. See :doc:`math/remote_heart_rate` before trusting any
         output number, and cross-check against a real reference (pulse
         oximeter, manual pulse count) if accuracy actually matters.

      **What it does**: estimates heart rate from subtle, camera-visible
      color changes in facial skin (remote photoplethysmography). No
      contact sensor is needed, but it is correspondingly less reliable
      than one. Detects a face-skin region per frame, combines its color
      signal into a pulse waveform, and extracts a BPM estimate per sliding
      time window.

      **Controls**: a **Backend** dropdown (**Green**: fast naive
      baseline, no motion compensation; **CHROM**: chrominance-based;
      **POS**: default, generally the most robust classical method),
      plus **Window** and **Hop** length in seconds (defaults 10s/2s) and
      a smoothing-windows spinbox. There is deliberately no frame-skip
      control here, unlike every other plugin: skipping frames would
      undersample the pulse signal itself below what's needed to resolve a
      heart rate at all.

      **Reading the output**: a BPM-over-time chart (toggle raw vs.
      smoothed), a stats readout (mean/median/min/max BPM, % of windows
      that produced a reliable estimate), and a quality badge combining
      signal-to-noise ratio and face-detection density into one
      Excellent/Good/Acceptable/Poor tier. Treat "Poor" as "don't trust
      this number." A debug overlay on the video shows the tracked
      face-skin ROI box plus a live BPM readout, so you can visually
      confirm the algorithm is tracking real skin, not hair or background.
      A window with insufficient face detection reports no BPM at all
      rather than a guessed one. This is expected on footage where the
      subject doesn't hold still facing the camera, not a bug.

   .. tab-item:: Face Dynamics

      **What it does**: counts what a face does over a recording: blinks
      (with their length), long eye closures, smiles and which of them are
      Duchenne smiles (the cheeks rise too), brow raises and quick brow
      flashes, overall expressivity, and head nods and shakes. Made for
      **interview mode** (one camera, a large face, about 50 fps); on room
      cameras faces are small and the frame rate low, so blinks and nods
      are rough there, and the log says so.

      **Controls**: **min conf** (face detection threshold) and
      **Annotated video** (also write a copy of each video with the
      measurements drawn on it; on by default). There is no frame skip: a
      blink is only a few frames long.

      **Reading the output**: the annotated video plays when it exists. Eye
      outlines turn red during a closure and the lips amber during a smile,
      an arrow shows where the head points, the current events appear in
      large type at the top right, and running counts at the top left. The
      **Metric** dropdown picks what the chart plots (eye openness, smile,
      brow raise, expressivity, head yaw, pitch, roll or speed). The stats
      line gives blinks per minute and median blink length, smiles and
      Duchenne smiles with the share of time spent smiling, brow raises and
      flashes, and nods and shakes. **Export events CSV** saves one row per
      event. The ``face_dynamics/`` folder (**Open output folder**) also
      holds a per-frame CSV with all 52 blendshapes. See
      :doc:`math/face_dynamics` for every rule and threshold.

   .. tab-item:: EEG/Trigger ↔ Frame Sync

      **What it does**: resolves every logged trigger event (keyboard,
      serial, parallel-port, e.g. an EEG amplifier's trigger-out cable)
      to its nearest captured frame in every camera, so you know exactly
      what each camera recorded at the instant of each external event.
      Unlike every other plugin on this page, this one runs **synchronously
      in the app itself**: no subprocess, no progress bar, just a
      near-instant table once you click Run (or automatically, if it's
      already been run for this session).

      **Reading the output**: one row per trigger event (elapsed time,
      wall clock, source, label, value) plus a resolved frame number and
      timing offset (``Δms``) for every camera. Click a row to seek the
      currently-selected camera's video to that frame. Export as CSV for
      use in external EEG-analysis tooling (e.g. MNE-Python). See
      :doc:`recording`'s "Aligning streams in Python" section for the
      manual/scripted equivalent, and why ``trigger.csv``'s
      ``elapsed_ns`` column (not ``elapsed_ms``) is the one safe to
      compare across files.

.. _motion-tracking-note:

.. note::

   **Motion tracking** runs from the **Session Browser**, not the Analysis
   tab. It operates on the whole session (centroid tracking + heatmap/
   trajectory plots), not a single-camera plugin result view. See
   :doc:`math/motion_tracking` for its detection/assignment math.

Calibration workflow
--------------------------

.. dropdown:: Intrinsic calibration (per camera)
   :open:

   Solves one camera's own lens parameters (focal length, principal point,
   distortion), which are needed before that camera's frames can be
   undistorted or used in any 3D computation, including gaze fusion.

   1. Open the **Calibrate** sidebar tab, then the **Intrinsics** inner tab.
   2. Select the camera, set checkerboard **Cols**/**Rows**/**Square size**.
   3. Move the checkerboard through varied positions/angles/distances,
      clicking **Capture frame** on each good view; aim for 20–30 views.
   4. Click **▶ Calibrate**; check the reported RMS error (< 0.5 px is
      excellent, > 2.0 px means recapture with better coverage).
   5. Click **Save calibration to settings**.

   Full detail, including the checkerboard-printing recommendations and the
   exact ``CalibrationData`` fields written, is in :doc:`calibration`.

.. _room-extrinsic-calibration:

.. dropdown:: Room (extrinsic) calibration (all cameras at once)

   Solves every camera's position/orientation in one shared room coordinate
   frame, which is needed to combine per-camera 3D signals (like gaze rays)
   across cameras. Uses a **ChArUco board** (not the plain checkerboard from
   intrinsics) captured simultaneously by multiple cameras at once, since it
   tolerates partial views when different cameras only see part of the board
   from their own angle.

   1. Complete intrinsic calibration for every camera you want included
      first, since extrinsic solving needs each camera's own lens
      parameters.
   2. Open the **Calibrate** sidebar tab, then the **Room (Extrinsics)**
      inner tab. Each configured camera gets its own live-thumbnail panel.
   3. Hold the ChArUco board somewhere visible to two or more cameras at
      once and click **▶ Capture shot**. A found/not-found dot updates per
      camera. Repeat, moving the board through overlapping camera pairs,
      until every camera has a path of shared shots back to camera 0
      (the fixed room-origin reference). 8+ varied shots is a reasonable
      starting point for 6 cameras.
   4. Click **▶ Solve**. The result table shows, per camera, whether it
      resolved and its reprojection RMS (px). A camera with no shared-shot
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

      Diarization's underlying pyannote models are gated on Hugging Face:
      you must accept both models' terms of use on huggingface.co *before*
      a generated token will work, not just create the token. A token that
      hasn't accepted the terms fails with an authentication error, not a
      missing-token warning.

   .. grid-item-card:: 📦  FER+ download trap

      The FER+ model file is served over Git LFS upstream. If a download
      ever produces a suspiciously small file (~130 bytes instead of ~35
      MB), it's an LFS pointer stub, not the real model. MOSAIC's
      downloader already guards against this with a sha256 check, but it's
      worth knowing if you ever fetch the model manually.

   .. grid-item-card:: 🖥️  GPU / CPU auto-selection

      Pose, diarization, and gaze fusion all auto-select CUDA if a working
      GPU is available, falling back to CPU otherwise, with no manual device
      flag needed. FER+, face-mask detectors, py-feat, rPPG, and 3D Pose
      Reconstruction all run CPU-only by design: the models are small
      enough (FER+, face-mask), the underlying library doesn't auto-select
      a device (py-feat), or there's simply no GPU-acceleratable step at
      all (rPPG's classical signal processing; 3D Pose Reconstruction's
      pure linear algebra). For all of these, a GPU wouldn't meaningfully
      help.

   .. grid-item-card:: 🧑‍🤝‍🧑  Subject identity across frames

      **Pose** tracks identity across frames (BoT-SORT), and **3D Pose
      Reconstruction** does its own nearest-centroid matching per tick, but
      neither is a guarantee: a long occlusion ends a track, and the person
      returns under a new id. The two numbering systems are also unrelated,
      so Pose's "Subject 2" and the room view's "track 2" are not the same
      label. Ids are per-video and per-run: never comparable between
      cameras, or between two analyses of the same footage.

      **Facial Expression** and **2D Gaze** still do *not* track identity.
      Their "subject 0" means "first detection in that frame", so their
      stats remain fully reliable only for single-subject sessions.

   .. grid-item-card:: ❤️  rPPG is experimental: verify before trusting

      Remote Heart Rate estimates are research-grade only, not clinically
      validated. They also need a subject holding reasonably still and
      facing a camera for the whole analysis window (10s by default).
      Footage where nobody looks steadily at a camera will correctly
      report no reliable estimate rather than a fabricated number. See
      :doc:`math/remote_heart_rate` for the full accuracy discussion.

   .. grid-item-card:: 🎬  py-feat's FFmpeg requirement

      The py-feat Facial Expression backend needs a torchcodec-compatible
      FFmpeg (versions 4–8, a shared/DLL build) discoverable on ``PATH``,
      even though it never touches video I/O directly. If this backend
      fails to construct, check ``where ffmpeg`` before suspecting a code
      issue.

   .. grid-item-card:: 📁  Where recordings actually live

      Each profile's sessions live under its own
      ``recordings/<username>/`` folder, resolved relative to wherever the
      app is running from, not a single shared folder. Only an admin
      profile sees every profile's sessions at once (see the note above);
      a regular profile's Record Settings directory field is read-only for
      exactly this reason.
