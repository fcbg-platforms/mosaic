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

      ``synced/``: every camera lined up frame by frame, one frame per
      trigger, with missed frames marked.

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
synchronisation quality. Shortly afterwards, **Frame Sync Repair** lines the
cameras up in the background, in about a second, and writes the result into
``synced/`` (see :ref:`synced-videos`). It never starts while a new recording
is running; it waits until that one stops. Closing MOSAIC before it is done
asks first, and a Frame Sync cut off this way is finished the next time
MOSAIC starts.

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
        about **21 fps**. The share is the **Trigger rate** setting under
        **Camera sync**; see :doc:`camera_settings` before raising it.
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
little early. After each recording, MOSAIC lines every camera up on the
triggers, in about a second: for each trigger, which frame of each camera
belongs to it, and which triggers a camera missed. That goes into
``synced/``, as one small table per camera. The analyses line the cameras up
through it; they never need copies of the videos.

To watch or share the cameras side by side, the Analysis tab's **Frame Sync
Repair** page has **Export synced videos**. It writes an aligned copy of every
camera's video into ``synced/``, encoded like the recording (on the graphics
card, at the original's bitrate): about the originals' size, made in about a
quarter of the recording's length. In those videos:

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
aligned, and says whether the synced videos have been exported. Running
Frame Sync again removes exported videos, which followed the old alignment:
export them again. Older sessions, and sessions without hardware triggering, are aligned
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

**Finding out what went wrong.** When MOSAIC did not shut down normally, the
next start of the profile that was in use (or of any admin) says so, once,
even if other profiles used MOSAIC in between. **Help > Problems on this PC** then lists it with every warning and
error MOSAIC has logged on this PC, newest first, kept across restarts: your
own profile's, or every profile's for an admin. A warning that repeats (once
per frame, say) is listed with how many times it fired instead of thousands
of times; a failed analysis names the plugin, the session and the end of its
error output. **Export…** saves what the list shows, with the logs, as one
``.zip`` to send to someone; an admin can add the crash dumps, which hold a
copy of MOSAIC's memory at the crash (participant names included).

Where things are kept, in the app-data folder (``%LOCALAPPDATA%\CSRU\MOSAIC``):

- ``problems.jsonl``: the problem journal, one entry per line, with
  ``problems.1.jsonl`` holding the older part once it passes 5 MB;
- each profile's ``mosaic.log`` (this run) and ``mosaic.previous.log`` (the
  run before, so the log of a run that crashed survives the restart);
- ``crashes\<profile>_<run>.log``: a copy of the log of each run that did
  not shut down normally, kept whatever happens to the profile's own logs;
- ``crashes\mosaic_crash_<date>-<time>.dmp``: a crash dump per crash, for
  a developer to open in Visual Studio or WinDbg.

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
       **Settings → Video**. That is only needed for a camera that failed to
       open. One that was running and dropped out (a pulled cable) reconnects
       by itself within a few seconds of coming back, even during a
       recording; meanwhile the pre-flight dialog says it **has dropped out**.
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
  the ceiling, and triggered recording runs at 85 % of the slowest camera
  (the **Trigger rate** setting).
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
rate. On-target is the share of frames with a face where the gaze offset is
within 0.35 of straight at the camera, the same rule the Gaze 2D plugin
uses; it shows ``--`` when no face has been seen for 5 seconds. An **Analyze** checkbox per tile takes one camera out of live analysis
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
- **Make a session report:** **📄 Session report** writes one page with
  everything the analyses found and opens it in your browser
  (``report/session_report.html``; print it to PDF to share). It shows:

  - a bar of links to its sections, kept at the top while scrolling;
  - the session's details and notes;
  - headline numbers;
  - **data quality**, before any numbers are read: whether the recording
    ended cleanly, frames missing per camera, how much of the video a face
    was seen, how clear the heart-rate signal was, each camera's lens and
    room calibration, whether speakers were told apart, and, for 3D pose,
    identity jumps and swaps and how often each camera disagreed. Green is
    fine, amber means read the numbers with care, red is worth fixing first;
  - a time-aligned timeline per camera, one colour per lane: who speaks,
    eye contact (faint while looking away), blinks, smiles (a second shade
    for a Duchenne smile), brow raises, nods, head shakes, and heart rate with
    its scale, coloured by signal quality as in the Analysis tab;
  - a section for each analysis, with the response-time distribution and
    eye contact while speaking, listening and in silence. An empty value
    says why (for example, run Eye Contact again after Conversation Timing);
  - the transcript itself, with times and speakers, and the voice
    spectrogram with its pitch track;
  - the analyses not run yet, so run those first and make the report again.

  The page prints cleanly to PDF (the link bar is left out, the transcript
  printed in full). From a terminal, ``python analysis/run_session_report.py
  --session <folder> --stills`` also adds three still frames from each
  video; they show the participants, so the app's button leaves them out.

  It also writes the session's numbers as one row
  (``report/session_summary.csv`` and ``.json``).
- **Combine sessions for statistics:** **📊 Summary CSV** at the bottom of
  the list writes that row for every session the list shows (an admin's
  includes the other profiles' sessions) into one ``sessions_summary.csv``
  in your recordings folder, and opens it. Sessions with one camera
  (interview mode) share the same column names; sessions with several
  cameras get one set of columns per camera (``cam1_…``, ``cam2_…``). Close
  the file in Excel before running it again.
- **See its state at a glance** from the coloured badges:

  - **INTERRUPTED** (red): the recording never finished; its files end where
    MOSAIC stopped.
  - **RECORDING** (green): it is being recorded right now.
  - **ARCHIVED** (grey): it has been moved to your archive folder (see
    below).
  - **SYNCED**: Frame Sync has lined the cameras up (``synced/``).
  - **POSE**, **MOTION**, **TRANSCRIPT** (Speaker Diarization),
    **EXPRESSION**, **GAZE**, **3D POSE**, **HR** (Remote Heart Rate),
    **GAZE 2D**, **FACE DYN**, **TURNS** (Conversation Timing), **EYE CONTACT**: that analysis has
    been run. Face Masking has no badge of its
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

Archiving sessions
~~~~~~~~~~~~~~~~~~

Six cameras fill a drive quickly. **🗄 Archive…** at the bottom of the
Session Browser moves finished sessions to an archive folder, ideally on
another drive or a network share, to free the recordings drive.

1. **Choose…** the archive folder once. It is saved with your profile's
   settings; it cannot be inside your recordings folder.
2. The dialog lists your sessions with their age and size, and shows the
   free space on both drives. Sessions older than the **Suggest sessions
   older than** age (30 days by default) are ticked for you; tick or untick
   any session. **Tick them** applies the age again.
3. **Archive** moves the ticked sessions one after another. Nothing moves
   until you click it. **Cancel** stops at once; the session being copied
   stays where it was, and the ones already moved stay archived.

Each session is moved safely:

- every file is copied, then read back from the archive and compared with
  the original (size and SHA-256 checksum); any difference stops the move
  and keeps the original;
- ``archive_manifest.json`` in the archived folder lists every file with its
  checksum, so the copy can be checked again later;
- the original is deleted only after that, and only if nothing in it
  changed while it was being copied (an analysis still writing, for
  example); otherwise everything is put back as it was and nothing is
  moved;
- if one of its files is open (a video playing), the move usually stops
  with nothing moved: close it and try again. If Windows lets the move
  through anyway, the part of the original it cannot delete is left in a
  ``.removing`` folder and MOSAIC says so;
- a session holding a link or a junction (a folder that points elsewhere)
  is refused, so nothing outside the session is ever copied or deleted;
- a session being recorded, here or on another PC sharing the folder, cannot
  be archived, and **Archive** is refused while an analysis is running.

Archived sessions stay in the Session Browser with an **ARCHIVED** badge
while the archive folder is reachable, and every analysis can still run on
them. **Move back** in a session's details moves it into your recordings
folder again, checked the same way. Several profiles can share one archive
folder: each sees only its own sessions there, and an admin sees them all
(but can move back only their own).

A folder ending in ``.partial`` (a copy that was interrupted; its name
includes the computer that made it) or
``.removing`` (an original whose deletion was interrupted) is never shown
as a session. Once you have checked that the session itself is fine, you
can delete such a folder.

.. _running-an-analysis-tab-plugin:

Running an Analysis-tab plugin
------------------------------------

Every plugin below (except Motion) lives on the top-level **Analysis** tab,
sitting alongside **Live**. The workflow is always the same shape:

1. Pick a session from the left-hand session list.
2. Pick a plugin from the plugin dropdown.
3. Set that plugin's controls (model/backend/etc.; see the tab below for
   each one). For a plugin that analyses each camera on its own (Pose, Face
   Masking, Facial Expression, Heart Rate, 2D Gaze, Face Dynamics,
   Conversation Timing, Eye Contact, Object Tracking), **Cameras** next to
   **Run** picks which cameras to analyse, all by default: untick the others
   to save time, for example to track objects only on the camera that sees
   the table. The other cameras' results stay as they were. Every camera is
   ticked again when you pick another session.
4. Click **Run**. Progress streams into the log view; you can queue another
   session's run while one is in progress, since jobs serialize through one
   shared subprocess queue.
5. Once finished (or immediately, if the session already has that plugin's
   output), the results panel loads automatically.

.. tab-set::

   .. tab-item:: Session Overview

      **What it does**: puts one camera's analyses on a single timeline,
      one lane each, so their moments can be read against each other: who
      speaks (on camera above, the other speaker below; from Conversation
      Timing), eye contact (looking at the partner, or away; from Eye
      Contact), smile and head movement (from Face Dynamics), and heart rate
      (from Remote Heart Rate). It reads the files those analyses already
      wrote: there is nothing to run, so it has no Run button.

      **Reading it**: the red line is the playhead, moving with the video;
      click anywhere on the lanes to jump there. Hover to read every lane at
      that moment (the time, who speaks, looking or away, the smile, head
      speed and heart rate). Each line lane shows its range at the left
      (smile 0 to 1; head movement up to the speed the head stays under 99% of
      the time, at least 10 degrees per second; heart
      rate rounded to 10 bpm). A lane whose analysis was not run for this
      camera says which one to run; run it, then come back to the overview.

   .. tab-item:: Object Tracking

      **What it does**: finds objects you name, "toy, cup, tablet computer" by
      default, in each camera's video with SAM 3, follows each one (its own
      id, so two cups are told apart), and when a hand touched which object
      and for how long. It follows hands too, so touches are found on a
      close-up camera that sees only an arm; when Pose was run on that
      camera, it also says whose hand it was. Writes
      ``objects/video_N.objects.json`` and, with **Annotated video**,
      ``objects/video_N.objects.mp4`` with every object's outline and name
      (thicker while touched) and the hands in grey. See
      :doc:`math/object_tracking`.

      **Before the first run**: download the SAM 3 weights, ``sam3.pt``, from
      Hugging Face (``facebook/sam3``; you must accept Meta's licence there
      first) and put the file in the MOSAIC folder. It needs the GPU, and takes about 8 times the
      recording's length per camera at every second frame.

      **Settings**: **Objects**, short names separated by commas, one thing
      each ("toy", not "toys") and clear ("tablet computer": "tablet" alone
      also finds door panels); **every N frames** (2 by default: half the
      time, and hands and objects are still followed).

      **Reading it**: **Show** picks when each object is seen, or when it is
      touched, one row per object. The line under the chart gives, per
      object, how much of the video it was seen in and who touched it for
      how long: "someone" when Pose was not run on this camera or does not
      see the person. **Rename objects…** gives the objects found names of
      your own ("red ball" for "toy 2"). They are saved with the camera's
      result and shown as "red ball (toy 2)" in the chart, the line under it
      and the session report; the annotated video keeps "toy 2". Running
      Object Tracking again starts the names over, as its numbers can change. The chart always spans the whole video. When no touch
      was found, its title says so; a result from before hands were followed
      asks you to run Object Tracking again.

   .. tab-item:: Social Interaction

      **What it does**: measures how two people's behaviour is coupled, each
      seen by the camera facing them: whether their head movement, body
      movement and heart rate move together (and who leads), and whether one
      smiles or nods soon after the other more often than chance. It reads
      what Face Dynamics, Pose and Remote Heart Rate wrote for those two
      cameras, so run those on both first. Needs a room recording of at
      least a few minutes (23 s at the very least for movement, 101 s for heart
      rate); interview recordings show only one person and are refused. See
      :doc:`math/social_interaction`.

      **Settings**: **Person A** and **Person B**, the camera facing each
      (Camera 2 and Camera 5 by default). If a camera does not face its
      person, the notes say so and name it.

      **Reading it**: **Show** picks a measure. *Synchrony* plots each 5 s
      window's peak correlation (30 s for heart rate); the grey line is what
      chance gives 95% of the time, so windows above it are coupled. *Who
      leads* plots the lag of each window's peak: above zero, B follows A.
      The line under the chart gives the mean against chance (and its p), who
      leads, the smile and nod mimicry (how often one followed the other
      within 3 s, against chance), and what could not be measured and why.
      Results are in ``social/social.json`` and, per window,
      ``social/synchrony.csv``.

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
      each person so you can watch this directly. When the tracker loses
      someone for a moment (a person walking past in front of them) and
      picks them up again under a new id, the pieces are joined back: a
      track that starts within 5 s of another's end, in the same place,
      keeps that track's Subject number. Three limits remain: someone who
      leaves the frame for long enough, or comes back somewhere else,
      returns as a *new* Subject (each stats line shows the time span it actually covers, so a
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

         The token is entered once per PC: every profile uses the same
         one, so nobody needs to enter it again. Strictly it is once per
         Windows account, since it is stored encrypted for the account
         MOSAIC runs under; on a PC where every profile runs under one
         account, as in room 11, that is the same thing. Tick **This
         profile only** beside the field to give one profile its own token;
         the others keep the shared one. Emptying that profile's own token
         unticks the box and goes back to the shared one. A profile that
         already had a token from before shares it with the PC the first
         time it logs in, if the PC had none.

         MOSAIC will not start a run that cannot produce speaker labels: if
         the token field is empty and "Transcript only" is unticked, Run
         stops immediately rather than spending minutes on transcription to
         reach an unlabelled result. Tick "Transcript only" to transcribe
         without speakers deliberately.

      **Acoustics** (spectrogram, pitch and intensity under the waveform)
      has its own tab: see **Voice Acoustics**.

      **If the Speaker column is blank**, a banner above the waveform says
      why (no token, terms not accepted, the model failing to load, or the
      model running and finding no speaker turns), together with what to do
      about it. That reason is recorded in the transcript file itself
      (``diarization_status``), so it is still available long after the run
      log has scrolled away. Transcripts produced by older versions have no
      such record; those report only that labels are missing.

   .. tab-item:: Voice Acoustics

      **What it does**: draws, under the session's audio waveform, a
      **spectrogram** (time across, frequency up, loudness as brightness),
      the **pitch** of the voice (cyan, on a logarithmic scale) and the
      **intensity** contour (amber), with the same speaker colours marking
      each diarized turn. It is a time-frequency picture of the sound: vowels
      show as bright horizontal bands (formants), hissing sounds as noise
      high up, pauses as dark stretches.

      **How to run**: the **▶ Acoustics** button beside the mic picker in the
      Speaker Diarization view. It is a separate, fast pass
      (praat-parselmouth): it needs no Hugging Face token and does not
      re-transcribe, so a session that was never diarized can still be
      examined acoustically, and getting a spectrogram never costs another
      Whisper run. Results are cached beside the audio as
      ``<mic>.voice.json`` and ``<mic>.voice.png``, so re-opening the session
      is instant. **Pitch** and **Level** toggle the two overlays.

      The spectrogram shares the waveform's time axis exactly, so the
      playhead and every speaker turn line up between the two strips;
      clicking either seeks playback. It shows 0-5 kHz by default, Praat's
      own default view and where the formants of speech lie. A gap in the
      pitch line is not missing data: the frame was unvoiced (an [s], a
      pause), and the line is deliberately broken rather than drawn across
      it. Levels are relative to the microphone's gain: compare within a
      recording.

      See :doc:`math/voice_acoustics` for what a spectrogram shows, the
      window and every other setting, how to read it, and its limits.

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
      bounding box + dominant-expression label per detected face. The
      **Show** dropdown picks the chart: the **share of time per
      expression** (bars, % of the frames with a face), the **action units'
      mean intensity** (bars, py-feat backend only), or one blendshape's
      score over time. The chart's time axis always spans the full analyzed range of
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
      combine cameras. It works best on a session whose synced videos have
      been exported (Frame Sync Repair's **Export synced videos**), and uses
      the raw videos otherwise.

      **Controls**:

      - **min cams**: cameras that must see a person before their gaze is
        reported (default 1).
      - **min conf**: face detector threshold.
      - **skip**: analyse every Nth frame (default 2; the videos still show
        every frame).
      - **subjects**: how many people to keep (auto keeps everyone seen for
        at least a second).

      **While it's running**: it works in three steps, named on the upper
      bar ("Step 1 of 3 · Finding faces", then "Fusing the cameras", then
      "Writing the videos"); the lower bar is the progress of that step, so
      it starts again at 0 for each. Faces are found in every camera at
      once, so the log says which camera is done and how many are.

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
      - One line per person: a face that only one camera sees, where the
        other cameras already place someone, is that person, not a second
        one, and a person split for a moment is joined back (see
        :doc:`math/gaze_fusion`). Two lines from one face are most often
        a camera off its calibration: check the room calibration and
        recalibrate if a camera is off.
      - The stats line gives, per person, how much of the time a gaze was
        found and what they looked at most, plus the share of time with
        mutual gaze.
      - **Export CSV** writes one row per frame and person.
      - The **Charts** tab, next to **Room**, shows over the whole
        recording, in **Show**: **Where a person looks** (choose the person:
        one row per target, high while they look at it, the most looked at
        lowest; with *nothing* for a gaze landing on no target and *no gaze*
        when the eyes were not usable), **Mutual gaze** (one row per pair,
        high while both look at each other's face, with the total, the
        number of episodes and the longest), or **Who looks at whom**, a
        table of each person's time on each target and its share of the
        time they were seen. **Export summary CSV** saves those totals,
        every stretch of looking at a target and every mutual gaze episode.
        See :doc:`math/gaze_fusion` for how the time is counted.

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

      **Reading the output**: the selected camera's video stays on the
      left, and two tabs choose what fills the space beside it. **3D Room**
      is an interactive room view (drag to orbit, scroll to zoom,
      double-click to reset) with a floor grid, every calibrated camera's
      position, and every person's 3D skeleton, one colour per person
      ("Person 1", "Person 2"). **Charts** shows, over the whole recording,
      one person's **speed**, **head and hip height** above the floor (standing,
      sitting and bending show), the **cameras** seeing them, their
      **posture** (sitting, standing or walking, one row each, from the
      hips; nothing where the hips were not seen), or a joint angle
      (**knee**, **hip** and **elbow flexion** and **arm raise** at the
      shoulder, left and right; all 0 standing upright with the arms hanging,
      see :doc:`math/pose3d_reconstruction`), or how they move: **hand
      speed**, **knee** and **elbow bending speed**, **whole-body motion** and
      the **head**'s speed and turning, and with two
      people their distance, approach, facingness and congruent motion
      (**Two people: …** in **Show**). The line under the chart gives how long
      the person was in 3D, their top speed, their usual head height, on the
      posture chart the seconds in each posture and, on
      an angle chart, the usual (median) angle, on a movement chart the usual
      and a high value (95th percentile);
      **Export CSV** saves the chart's numbers. **Export everyone (CSV)**
      saves one row per tick and person with every one-person measure
      (``people.csv``), and, when the session has a 3D Gaze result, what that
      person looks at and whether it is mutual gaze: the table to load for
      statistics. A 3D Gaze person is matched to a 3D Pose person by where
      their heads are (see :doc:`math/pose3d_reconstruction`); with no sure
      match the gaze columns stay empty. MOSAIC also writes each person's
      summary (``pose3d_people.json``: time in each posture, usual joint
      angles and movement) when it opens a 3D Pose result or a run finishes
      here, and the session report shows it under **3D Pose, per person**.
      A **Person** dropdown filters
      the 3D Pose CSV export to one person or "All people". The selected camera's
      video also shows the reprojected 2D skeleton overlay, letting you
      visually confirm the 3D reconstruction actually lines up with what
      that camera really saw. The run's log, and the session report (a
      coloured strip under 3D Pose, and a row in Data quality), say how much
      of the recording (with someone in view) had everyone in 3D, and why
      people were missing the rest of the time: no camera frames, only one
      camera saw anyone, someone seen by one camera only or not matched
      across cameras, or cameras that did not agree on their keypoints. See
      :doc:`math/pose3d_reconstruction` for the
      triangulation, cross-camera association, and tracking math, and for
      its important caveat about 2-camera rigs with multiple people.

      **The room view**: people are drawn as shaded limbs and joints with a
      filled torso and a head, upright on a lit floor with a grid every 0.5 m
      (brighter each metre), with real shadows from a light above. A small
      cone in front of each head shows which way the face points. Each
      camera is drawn as a small camera, its lens in its colour, with what it
      sees as a coloured pyramid. The buttons at its top right choose the view: **3D** (drag to
      turn, right-drag to pan, scroll to zoom), or **Top** (a flat map of the
      floor), **Front** and **Side**, flat views where dragging pans.
      Double-click returns to the view's starting framing. Click a camera's
      label to see the room from that camera: from where it is, looking
      where it looks, with its vertical field of view, to compare with its
      video (without its lens distortion; the view's shape, not the camera's,
      sets how much shows at the sides). Any view button, or a double-click,
      leaves it. **Trails** shows where each person's hips were over the last
      2 seconds, and their trace on the floor. **Paths** draws each shown
      person's whole path on the floor over the recording, where they went
      and stood, broken where they were not in 3D. **Save image** saves the
      view as drawn as a PNG, named after the tick on show, in the session's
      folder by default. Hover over a joint to read its name and position, x, y
      and z in metres, in the room frame: the reference camera's own (x
      right, y down, z forward), as in every export; the axes at the bottom
      right turn with the view. Results from before this version have no
      camera directions: run 3D Pose again to see the pyramids. The
      **People** panel at its top left lists everyone with how long they
      were in 3D: click a person's colour box to hide or show them, and
      **Follow** keeps one in the middle of the view as the video plays.
      Someone in 3D for under a second is marked **(brief)** and starts
      hidden: more often a passing false detection than a person. Brief
      people are also left out of the **Person** pickers (unless nobody
      lasts a second); "All people" in the CSV export still includes them.
      The view is drawn with Qt Quick 3D; if it cannot load (Qt Quick 3D
      missing next to MOSAIC), it says so, and **Help > Problems on this PC**
      has the error.

      **People and tracks**: the tracker follows each person from frame to
      frame, and loses them when too few cameras see them for a few frames
      (5 ticks, 0.2 s at 25 fps); they come back as a new *track*. 3D Pose then joins the
      tracks that are clearly the same person (the next one starts within 3
      seconds, close to where the last ended) into one **person**, so one
      person who steps out of view for a moment stays "Person 1". Two people
      seen at the same time are never joined. Results from before this
      version show one person per track: run 3D Pose again.

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

      **Beats and heart-rate variability (HRV)**: the plugin also finds
      every heartbeat and reports HRV: RMSSD, SDNN, pNN50 and LF/HF. The
      chart dropdown switches between the windowed heart rate, the
      beat-to-beat heart rate, RMSSD over a sliding minute, and the signal
      quality. The windowed rate is drawn as steps covering the whole
      video, each step the rate of the window around it; gaps are windows
      without a reliable face. Each step is coloured by its window's pulse
      signal quality, with the quality badge's tiers: excellent (green, SNR
      5 dB or more), good (yellow-green, 0 to 5 dB), acceptable (amber, -5
      to 0 dB) and poor (red: below -5 dB, or the face found in under 60%
      of the window's frames). Trust the green stretches most. **Signal quality (SNR, dB)** plots that number
      itself. The beat-to-beat view shows every detected beat in a
      dim line and, over it, the clean beats used for HRV. Beats need at
      least 10 s of face without a break of over half a second, so a
      stretch where the face was hidden, and a short stretch around it,
      has no beats. RMSSD needs 60 s of beats per value: a shorter
      recording has none, and the chart title says so. Camera
      timing noise inflates HRV. The plugin measures that noise by finding
      the beats separately in the two halves of the face, and the stats
      line gives the noise-corrected RMSSD next to the raw one. HRV is not
      reported below 25 fps (so not on room cameras), or with under a
      minute of clean beats; the stats line says why. With
      **Conversation Timing** run first it also gives the mean heart rate
      while speaking and while listening. Two more charts: **Heart rate:
      speaking, listening, silence** (bars of the mean rate from the clean
      beats in each, from this camera's Conversation Timing; a beat during
      overlapping speech counts as both speaking and listening, as in the
      stats line; a state needs 10 clean beats), and **Beat to beat (Poincare plot)**, each clean beat
      interval against the next: a tight cloud along the diagonal is a
      steady heart, its width across the diagonal (SD1, in the title) the
      beat-to-beat variability. Interview mode at 50 fps with
      even light gives the best results. See :doc:`math/remote_heart_rate`.

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
      brow raise, expressivity, head yaw, pitch, roll or speed), the blinks
      per minute of face seen over time (in equal windows of about 30 s, like
      the stats line; a window with under 5 s of face is left as a gap),
      where the head points (each frame's yaw against its pitch: a tight
      cluster is a still head, a wide one a moving or turning head), or
      the events
      as a timeline, one row each for blinks, smiles, brow raises, nods and
      head shakes. The stats
      line gives blinks per minute and median blink length, smiles and
      Duchenne smiles with the share of time spent smiling, brow raises and
      flashes, and nods and shakes. **Export events CSV** saves one row per
      event. The ``face_dynamics/`` folder (**Open output folder**) also
      holds a per-frame CSV with all 52 blendshapes. See
      :doc:`math/face_dynamics` for every rule and threshold.

   .. tab-item:: Eye Contact

      **What it does**: measures how much of the time the person on a camera
      looks at their conversation partner, and when, for how long and in
      which direction (up, down, their left or right) they look away. With
      **Conversation Timing** run first, it also splits eye contact into
      speaking and listening, and checks the answers: how often the person
      looks away as they start answering, looks back as they finish, and
      looks away while thinking before an answer.

      The partner's direction is found from the gaze itself: where the
      person looks most while listening. That cancels the gaze estimate's
      constant errors, but it assumes there is a partner to look at. Choose
      **Partner: the camera** for a remote interview, or when the
      interviewer sits right beside the camera. That measure is absolute,
      so errors of the gaze estimate are not cancelled.

      **Controls**: the **Partner** choice and **Annotated video** (the gaze
      arrow, the state, and a small map of the gaze around the partner).

      **Reading the output**: the **Metric** dropdown shows eye contact over
      time (1 looking at the partner, 0 away), eye contact while the person
      on camera speaks, listens and while nobody speaks (bars; it needs
      Conversation Timing run first, then Eye Contact again), how long the
      look-aways lasted (a histogram: 0 to 0.5 s, 0.5 to 1 s, and so on up to
      16 s and more), the gaze around the partner (each frame's gaze
      relative to the partner's direction, with the contact cone as a
      dashed circle: points inside it are roughly eye contact, which is
      decided on the exact angle with some smoothing; to the right is the
      image's right, the person's left), the angle from the partner, or the gaze yaw and pitch.
      The stats line gives the
      overall eye contact share, listening, speaking and silence shares, the look-aways and their
      directions, the answer patterns, and where the partner was found.
      **Export look-aways CSV** saves one row per look-away. See
      :doc:`math/eye_contact` for every rule.

   .. tab-item:: Conversation Timing

      **What it does**: times the conversation between the person on a
      camera (in interview mode, the interviewee) and everyone else heard on
      the microphone (the interviewer):

      - who speaks when;
      - how quickly each answers (the *floor transfer offset*: the gap, or
        the overlap when one starts before the other stops);
      - interruptions, pauses within a turn, and short "mm-hm"
        backchannels;
      - speech time and, with a transcript, words per minute.

      Speech timing comes from the audio; the camera (mouth movement) decides
      whose speech it is. Run **Speaker Diarization** first if you can: its
      speaker labels make attribution more reliable, they are needed to see
      overlapping speech, and they add the words of each turn. Running **Face
      Dynamics** first saves this plugin a pass over the video.

      Recordings made with this version also save an audio timing file, so
      sound and picture line up to within the sound card's own capture
      latency (typically tens of milliseconds). For older recordings
      the plugin lines them up from lip movement where it can, and says so.

      **Controls**: **Use diarization** (use the speaker labels when the
      session has them) and **Annotated video** (a copy of the video showing
      who is speaking, the last response time, the current turn's words and
      a scrolling timeline; it has no sound).

      **Reading the output**: the **Metric** dropdown shows who is speaking
      over time (on camera above the axis, the other speaker below), each
      response time as a point, the response times as a histogram (0.25 s
      bins: below 0 the next speaker started before the other stopped,
      around 0.2 s a quick answer, beyond a second a long gap), the audio
      level, the mouth movement, how the recording's time divides (on camera
      only, the other only, both at once, silence), how long the turns
      were, per speaker (a histogram, finer for short turns), or the speech
      rate of each turn in words per minute (from the transcript words, so
      it needs Speaker Diarization; turns under 1 s are left out). A
      transcript segment's words all go to the turn holding its middle, so a
      short turn can show a very high rate next to turns with none. With no change
      of speaker the
      response-time charts are empty and their title says so. The
      stats line gives each speaker's speech time, turns and median response
      time, the count of speaker changes split into gaps, overlaps and
      interruptions, pauses and backchannels, and how speakers were
      attributed. **Export turns CSV** saves one row per turn with its
      response time and words; ``conversation/`` (**Open output folder**)
      also holds a transitions CSV. See :doc:`math/conversation_timing` for
      every definition and its accuracy.

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

      ``uv sync`` in ``analysis/`` installs PyTorch's CUDA build on Windows
      and Linux (it needs an NVIDIA driver from the 580 series or later, and
      falls back to the CPU without one). Everything built on PyTorch then
      uses the GPU on its own, with no device flag: **Pose** and its depth
      option, the YOLO face detector and segmenter of **Face Mask**, and the
      speaker-turn step of **Speaker Diarization**. On room 11's RTX A4000,
      Pose on a 1080p video went from 14.6 to 22 fps with the nano model and
      from 6.3 to 22 fps with the medium one, and depth from 10.7 to 28 fps.
      Video decoding is then the limit, so a larger Pose model costs little.

      **Transcription** (faster-whisper) has its own CUDA requirement: it
      uses the GPU only when it can load the CUDA 12 cuBLAS library (on
      Windows from PATH or from the ctranslate2 package folder; on Linux it
      also needs cuDNN 9), and the CPU otherwise. The log line ``Loading
      whisper model (... device=...)`` says which.

      The other analyses run on the CPU: MediaPipe face and gaze models
      (Gaze 2D, Gaze Fusion, Face Dynamics, Eye Contact, rPPG), FER+, and
      py-feat, which is set to the CPU. ``scripts/doctor.ps1`` reports
      whether PyTorch can use the GPU.

   .. grid-item-card:: 🧑‍🤝‍🧑  Subject identity across frames

      **Pose** tracks identity across frames (BoT-SORT), and **3D Pose
      Reconstruction** does its own nearest-centroid matching per tick, but
      neither is a guarantee. Pose joins a track back to one that ended up to
      5 s earlier in the same place, so a person briefly hidden keeps their
      Subject; a longer absence, or a return elsewhere, gives a new id. The two numbering systems are also unrelated,
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
      ``recordings/<username>/`` folder in MOSAIC's data folder: the checkout
      it runs from (``D:\mosaic\recordings\<username>`` in room 11), or
      ``Documents\MOSAIC`` for an installed copy, whichever way MOSAIC is
      started. (Before, a relative folder followed wherever MOSAIC was started
      from, so sessions could land inside the build output, where a rebuild
      deletes them. When MOSAIC finds sessions there it offers, at login and
      before the cameras start, to move them into the data folder: on the same
      drive each is simply moved, otherwise copied and checked before the old
      copy is removed. A profile moves its own; an admin, every profile's.) Only an admin
      profile sees every profile's sessions at once (see the note above);
      a regular profile's Record Settings directory field is read-only for
      exactly this reason.
