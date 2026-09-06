# Verification checklist

A living list of "verified on real hardware / a real display" checks for Mosaic. Most of this
codebase's automated tests only cover pure logic (`ctest`/`pytest`) — a lot of real behavior
(camera sync, GenICam node writes, paint/animation code, hardware-facing triggers, actual model
accuracy) can only be confirmed by running the app against real cameras/mics/EEG hardware or on a
real display. This file tracks what has and hasn't been checked that way yet.

**How to use this**: check an item off (`- [x]`) once you've personally verified it against real
hardware/a real display, ideally with a one-line note on when/how. Add new items here as new
features land with an open "needs real hardware verification" note — don't let that kind of note
live only in a PR description or a plan file where it'll be forgotten.

## Camera & sync

- [ ] White-balance manual lock (Red/Blue balance sliders) reproduces the same color across an
      app relaunch, and visibly shifts the live preview tint without needing to reopen the camera.
- [ ] Log panel now defaulting to "Error" severity, and the status-bar message removal, don't
      hide anything a user actually needs to see day-to-day.
- [ ] Keyboard-trigger dedup fix (state-based key-down/key-up tracking) actually suppresses
      whatever produced the original 9x-duplicate-rows bug — re-test on the same machine/keyboard
      that produced it.
- [ ] GigE Action Command continuous per-frame triggering (items 8b→8e→8f) holds up over a longer
      (multi-minute) recording, not just the short test sessions checked so far.
- [ ] Camera 5's Ethernet-6 cable/connector has been physically reseated or swapped, and a
      follow-up recording confirms the packet-loss rate actually improved.

## Recording & triggers

- [ ] EEG parallel-port trigger *output* marker (recording start/stop) actually changes voltage
      on the INIT pin (pin 16) — confirm via multimeter or the EEG software's own trigger-channel
      display, and confirm a real EEG recording shows the marker bracketing the exact Mosaic
      recording window.
- [ ] Keyboard trigger rebinding (changing the bound key while the app is running) takes effect
      immediately without needing a restart.
- [ ] `trigger_frame_map.json`/CSV export (EEG/Trigger↔Frame Sync plugin) resolves to the frame
      that visually matches the real-world event that fired the trigger — scrub the reported frame
      and confirm by eye.
- [ ] Start countdown: clicking Record shows a 3-2-1 overlay and capture begins on 0; the session
      folder's name-timestamp matches when the countdown *ended*, not when it was clicked (this is
      the check that the delay really is invisible to every recorded timeline); clicking again
      mid-countdown cancels it and leaves no session folder behind; **Ctrl+R / Ctrl+.** behave
      identically to the button; setting the delay to 0 restores instant start exactly.
- [ ] Hidden previews: the camera grid disappears the moment the countdown starts and the liveness
      strip shows one chip per camera; unplugging one camera darkens only that chip; every preview
      returns on Stop (one blank frame on re-show is expected — the Image source is cleared while
      hidden); toggling "Hide camera previews while recording" *during* a recording takes effect
      immediately.

## Calibration

- [ ] Intrinsic calibration end-to-end on all 6 room-11 cameras (has this ever actually been done
      on the current camera set, not just historically).
- [ ] Room (Extrinsics) calibration — capture ≥8 ChArUco shots across overlapping camera pairs,
      solve, confirm reasonable reprojection RMS (<2px) and camera 0 stays identity.
- [ ] "Use last shot as plane" + Save to settings, then confirm Gaze Fusion's target-point
      intersection looks physically sensible in the room view.

## Analysis plugins

- [ ] Face Masking — all 3 backends (MediaPipe, YOLOv8, OpenCV/YuNet) confirmed working on a real
      multi-camera session (last confirmed working 2026-07-28 for all 3, re-check periodically
      since 2 of the 3 already broke once from upstream changes — a retired OpenCV API, a moved
      GitHub release tag).
- [ ] Facial Expression — all 3 backends (heuristic, FER+, py-feat) confirmed working end-to-end,
      including py-feat's FFmpeg/torchcodec dependency chain on a fresh machine.
- [ ] Speaker Diarization — a full run with a real HF token against a session with actual
      overlapping speech, confirming speaker attribution looks right, not just that it doesn't
      crash.
- [ ] Multi-Camera Gaze Fusion — real room-11 hardware run: capture a session with someone looking
      at known points on the target surface, confirm the fused 3D ray/target point looks
      physically sensible.
- [ ] Pose subject identity (BoT-SORT) — record two people who walk past each other so they cross
      and swap detection order. Confirm each skeleton's "Subject N" tag stays glued to the same
      person through the crossing, that the kinematics chart shows no speed spike at the swap, and
      that each stats line's time span covers the whole appearance. Also confirm a person entering
      mid-recording appears (their very first analysed frame is dropped by design — ultralytics
      only emits a track once it has been confirmed on a second frame), and that re-running Pose on
      the same session renumbers subjects rather than reusing the old ids.
- [ ] Pose tracker confidence floor — the tracker is deliberately fed detections down to conf 0.1
      while only conf >= the UI's threshold get written, so NMS now runs over a wider band. On real
      crowded/partly-occluded footage, confirm this did not introduce spurious identity splits (one
      person suddenly becoming two Subjects while clearly visible). If it did, the fix is to raise
      TRACK_INPUT_CONF in analysis/pose/human_pose.py toward the UI threshold.
- [ ] 3D Pose Reconstruction — real multi-person, multi-camera footage (not synthetic data): are
      tracks correctly separated per person, not merged/duplicated; does the interactive room view
      orbit/zoom smoothly.
- [ ] 3D Pose Reconstruction's temporal smoothing (`--smoothing-window`, the room view's "Show
      smoothed" checkbox): on a real multi-person session, confirm the smoothed skeleton actually
      looks less jittery over time without looking laggy/over-smoothed, and that the 2D video
      overlay stays unaffected (always raw, by design). Also spot-check the CSV export's new
      `x_mm_smoothed`/`y_mm_smoothed`/`z_mm_smoothed` columns against what the room view shows.
- [ ] **rPPG (remote heart-rate) — the single highest-value unchecked item on this whole list.**
      Compare the estimated BPM against a real reference (pulse oximeter or manual pulse count) on
      a real subject sitting still under normal lighting. The entire feature's value proposition
      depends on this number being roughly right, and it has never been checked against ground
      truth.
- [ ] rPPG debug overlay — confirm the ROI boxes actually track forehead/cheek skin, not
      hair/background, on real footage.
- [ ] 2D Gaze (calibration-free) — run against a real recorded session with a visible face;
      confirm the bbox+direction-arrow video overlay tracks the subject's real gaze direction,
      the dx/dy/magnitude chart populates plausibly, and the stats readout (`% frames with face`,
      mean dx/dy, `% on target`) looks sensible. Confirm the `SessionBrowserW` "GAZE 2D" badge
      (yellow, distinct from "GAZE"'s cyan Multi-Camera Gaze Fusion badge) appears correctly and
      the two plugins' outputs aren't cross-classified (both write files containing the substring
      "gaze").

## Real-time tab

- [ ] Live pose/gaze tiles render correctly and track a real person entering/leaving frame within
      ~1s.
- [ ] Per-camera "Analyze" checkbox correctly narrows the shared inference budget without
      affecting other cameras.
- [ ] Auto-pause during recording: paused banner appears within ~500ms of a real recording
      starting, every tile's overlay freezes (not blanks), and inference resumes automatically on
      stop.
- [ ] Live transcript panel: captions appear every ~1.5-2s while speaking, correctly pause during
      recording, and resume after — including the non-16kHz-mono resample path (most real mics
      won't natively negotiate 16kHz mono).

## Security

- [ ] HF token DPAPI round-trip: paste a real token, close the app, confirm `settings.json` shows
      a `dpapi:v1:...` blob (not plaintext); reopen and confirm it decrypts correctly.
- [ ] Export/Import Configuration (admin panel): confirm the new "token failed to decrypt on this
      machine" warning actually fires when importing a config exported from a different Windows
      account/machine, and stays silent for a same-account import.

## BIDS-style session naming

The regression risk of the whole feature is the session *ordering* change, so
do that one even if you skip the rest.

- [ ] **Backward compatibility.** With Subject/Session/Task all blank, record a
      session and confirm the folder name is exactly the old
      `yyyy-MM-dd_hh-mm-ss` form and `session_meta.json` has no `bids` key.
- [ ] **Ordering (the one that can regress).** Open the Session Browser and the
      Analysis tab with a mix of old timestamp folders and new BIDS folders and
      confirm the list really is newest-first — not grouped by subject. Repeat
      as an admin with extra directories configured, which previously showed
      one sorted block per directory.
- [ ] Fill in Subject/Session/Task, confirm the preview line under the fields
      matches the folder actually created, and that `session_meta.json` carries
      `bids: {sub, ses, task, run}`.
- [ ] Type a hyphen or accent into a label (`P-01`, `Müller`) and confirm the
      warning line explains the coercion and the preview shows `P01` / `Muller`.
- [ ] **Duplicate prompt.** Record the same subject/session/task twice.
      The second start must prompt *before* the countdown, offer `run-02`, and
      the created folder must actually use `run-02`. Check Cancel and
      "Change details…" both leave no folder behind.
- [ ] Delete `run-02` of three and record again — the new session must be
      `run-04`, never a reused `run-02`.
- [ ] **Overwrite guard.** Set Record settings → uncheck "add timestamp", leave
      the identity blank, and record twice. The second must land in `session_2`
      with a logged warning, not overwrite the first. (Before this change it
      silently overwrote.)
- [ ] Start a recording from a keyboard trigger and confirm it inherits the
      identity typed in the monitor (no dialog, correct run number).
- [ ] **Notes.** Type a note before recording; edit it *during* the recording;
      confirm `notes.txt` holds the edited text ~5s later. On Stop, edit it
      again in the Session Health dialog and confirm closing via the window's X
      still saves. Then search for a word from the note in the Session Browser.
- [ ] Confirm a long BIDS name elides (middle) in the Analysis session picker
      rather than being clipped, and that its tooltip shows the full name.
- [ ] **Note must not leak between sessions.** Record A with a note, stop,
      then record B without touching the box: B's `notes.txt` must not contain
      A's note (the monitor box clears on stop).
- [ ] Leave the Session Health dialog open, edit the same session's notes in
      the Session Browser and save, then close the health dialog with its X —
      the browser's note must survive.
- [ ] Type a word that appears **only** in a session's notes into the Session
      Browser search box and confirm that session is found (not everything
      hidden).

