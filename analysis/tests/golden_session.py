"""
The golden session: a small synthetic interview pushed through the analysis
plugins' own analysis and output code, committed under ``tests/golden/session``.

Two tests use it:

* ``analysis/tests/test_golden_session.py`` regenerates it and compares with
  the committed copy, so a change to any plugin's output (a key renamed,
  removed, or of another type; a headline number moved) fails CI until the
  golden copy is deliberately updated.
* ``tests/test_golden_session.cpp`` loads the committed files with the app's
  C++ result readers, so a reader that no longer understands what Python
  writes fails too.

Inputs are synthetic and seeded: a 72 s interview at 25 fps on camera 3 (index
2), two voices taking turns (energy-detected, so the result does not depend
on the Silero model version), a mouth that moves with the on-camera voice,
gaze that looks away when answering, blinks, smiles and nods, and a face
colour carrying a pulse with breathing-linked variability.

To update the committed copy after a deliberate output change::

    python analysis/tests/golden_session.py --update
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import shutil
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ANALYSIS = HERE.parent
GOLDEN = ANALYSIS.parent / "tests" / "golden" / "session"
sys.path.insert(0, str(ANALYSIS))

FPS = 25.0
DURATION_S = 72.0
CAM = 2
SESSION_START_NS = 5_000_000_000
VIDEO_START_NS = SESSION_START_NS + 120_000_000
AUDIO_RATE = 16000
#: Who speaks when (audio seconds): the interviewer ("I") asks, the subject
#: ("S", on camera) answers. One answer starts before the question ends.
SCRIPT = [
    ("I", 1.0, 7.0),
    ("S", 7.6, 16.0),
    ("I", 16.5, 22.0),
    ("S", 21.6, 32.0),
    ("I", 33.0, 40.0),
    ("S", 40.8, 52.0),
    ("I", 52.6, 58.0),
    ("S", 58.4, 70.0),
]

# Files the golden copy keeps (the plugins also write CSVs and the report its
# HTML page; those are views of the same data and would only add size).
KEEP = (
    "session_meta.json",
    "video/timestamps_cam2.csv",
    "video/video_2.mp4",
    "audio/audio.transcript.json",
    "face_dynamics/video_2.face_dynamics.json",
    "conversation/video_2.conversation.json",
    "eye_contact/video_2.eye_contact.json",
    "rppg/video_2.pos.rppg.json",
    "gaze2d/video_2.gaze2d.json",
    "report/session_summary.json",
)


def frame_times_s() -> np.ndarray:
    return np.arange(int(DURATION_S * FPS)) / FPS


def subject_speaking(t: np.ndarray) -> np.ndarray:
    return np.any([(t >= a) & (t < b) for who, a, b in SCRIPT if who == "S"], axis=0)


def listening(t: np.ndarray) -> np.ndarray:
    return np.any([(t >= a) & (t < b) for who, a, b in SCRIPT if who == "I"], axis=0)


# ── Session folder ───────────────────────────────────────────────────────────


def write_session(root: Path) -> None:
    (root / "video").mkdir(parents=True)
    (root / "audio").mkdir()
    meta = {
        "schema": "mosaic-session-v1",
        "bids": {"sub": "golden", "ses": "01", "task": "interview", "run": 1},
        "recorded_by": "ci",
        "session_start_utc": "2026-10-10T09:00:00.000Z",
        "session_start_elapsed_ns": SESSION_START_NS,
        "recording": {"mode": "interview", "interview_camera": CAM, "interview_fps": 25},
        "cameras": [{"index": CAM, "name": "acA1920-25gc", "width": 1280, "height": 720}],
        "session_end": {
            "utc": "2026-10-10T09:01:12.000Z",
            "duration_ms": 72000,
            "ended_cleanly": True,
        },
    }
    (root / "session_meta.json").write_text(json.dumps(meta, indent=1), encoding="utf-8")
    (root / "video" / f"video_{CAM}.mp4").write_bytes(b"")  # listed, never decoded
    with (root / "video" / f"timestamps_cam{CAM}.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["frame_id", "elapsed_ns", "wall_ns", "hw_timestamp_ns"])
        for i, t in enumerate(frame_times_s()):
            w.writerow([i + 1, int(VIDEO_START_NS + t * 1e9), 0, 0])
    segs = [
        {
            "start_ms": int(a * 1000),
            "end_ms": int(b * 1000),
            "speaker": "SPEAKER_00" if who == "I" else "SPEAKER_01",
            "text": "question words here" if who == "I" else "an answer with several words in it",
        }
        for who, a, b in SCRIPT
    ]
    transcript = {
        "source_audio": "audio.wav",
        "diarization": True,
        "diarization_status": "ok",
        "segments": segs,
    }
    (root / "audio" / "audio.transcript.json").write_text(
        json.dumps(transcript, indent=1), encoding="utf-8"
    )


def write_audio(root: Path, rng) -> None:
    """Two harmonic voices (energy-detectable) taking turns. Not kept."""
    from scipy.io import wavfile

    n = int(DURATION_S * AUDIO_RATE)
    x = rng.normal(0, 0.002, n)
    t = np.arange(n) / AUDIO_RATE
    for who, a, b in SCRIPT:
        sel = (t >= a) & (t < b)
        f0 = 120.0 if who == "S" else 210.0
        tt = t[sel]
        v = sum(np.sin(2 * np.pi * f0 * k * tt) / k for k in range(1, 8))
        x[sel] += 0.2 * v * (0.6 + 0.4 * np.sin(2 * np.pi * 4 * tt))
    wavfile.write(
        root / "audio" / "audio.wav", AUDIO_RATE, (np.clip(x, -1, 1) * 32767).astype(np.int16)
    )
    # The recorder's timing file: audio started 40 ms after the session.
    with (root / "audio" / "audio.timing.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["sample_count", "elapsed_ns"])
        for k in range(1600, n + 1, 1600):
            w.writerow([k, int(SESSION_START_NS + 40e6 + k * 1e9 / AUDIO_RATE + 1e6)])


# ── Face Dynamics ────────────────────────────────────────────────────────────


def face_dynamics(root: Path) -> None:
    import run_face_dynamics as rfd
    from face_dynamics.metrics import EXPRESSIVE

    t = frame_times_s()
    talking = subject_speaking(t - 0.12)  # video starts 120 ms after the session
    m = rfd.Measurements(len(t), 52)
    blinks = np.arange(3.0, DURATION_S, 4.3)

    class Face:
        pass

    for i, ti in enumerate(t):
        if 30.0 <= ti < 30.6:
            continue  # face lost briefly
        f = Face()
        blink = np.any((ti >= blinks) & (ti < blinks + 0.2))
        f.ear_right = f.ear_left = 0.06 if blink else 0.3
        nod = 8.0 * math.sin(2 * math.pi * (ti - 44.0) / 0.8) if 44.0 <= ti < 45.6 else 0.0
        f.yaw, f.pitch, f.roll = 3.0, -2.0 + nod, 0.0
        smile = 0.8 if (24.0 <= ti < 27.0 or 60.0 <= ti < 61.0) else 0.05
        shapes = {name: 0.05 for name in EXPRESSIVE}
        shapes.update(
            {
                "mouthSmileLeft": smile,
                "mouthSmileRight": smile,
                "cheekSquintLeft": 0.5 if 24.0 <= ti < 27.0 else 0.0,
                "cheekSquintRight": 0.5 if 24.0 <= ti < 27.0 else 0.0,
                "browInnerUp": 0.8 if 50.0 <= ti < 50.4 else 0.0,
                "browOuterUpLeft": 0.8 if 50.0 <= ti < 50.4 else 0.0,
                "browOuterUpRight": 0.8 if 50.0 <= ti < 50.4 else 0.0,
                "jawOpen": 0.05
                + (0.25 * abs(math.sin(2 * math.pi * 2.5 * ti)) if talking[i] else 0.0),
                "mouthLowerDownLeft": 0.0,
                "mouthLowerDownRight": 0.0,
            }
        )
        f.blendshapes = shapes
        f.box = (500.0, 200.0, 760.0, 520.0)
        f.nose = (630.0, 360.0)
        f.outline = np.zeros((52, 2))
        m.put(i, f)
    times_ms = (VIDEO_START_NS + t * 1e9) / 1e6
    a = rfd.analyse(m, (times_ms - times_ms[0]) / 1000.0)
    rfd.write_outputs(
        root / "face_dynamics",
        Path(f"video_{CAM}.mp4"),
        CAM,
        FPS,
        times_ms,
        m,
        a,
        {"min_confidence": 0.5},
        None,
    )


# ── Conversation Timing ──────────────────────────────────────────────────────


def conversation(root: Path) -> None:
    import argparse as ap

    import run_conversation as rc
    from conversation import vad

    vad.detect_speech_silero = lambda *a, **k: None  # energy detection: model-independent
    args = ap.Namespace(no_video=True, min_confidence=0.5, no_diarization=False)
    meta = json.loads((root / "session_meta.json").read_text())
    audio = rc.Audio(root / "audio" / "audio.wav", meta, True)
    rc.process_camera(root, root / "video" / f"video_{CAM}.mp4", CAM, audio, args)


# ── Eye Contact ──────────────────────────────────────────────────────────────


def eye_contact(root: Path, rng) -> None:
    import argparse as ap

    import run_eye_contact as rec

    t = frame_times_s()
    talking = subject_speaking(t - 0.12)
    g = rec.GazeTrack(len(t))
    # On the partner (25, -5) while listening; away (up) for the first 3 s
    # of each answer and now and then while answering.
    phase = np.zeros(len(t), bool)
    for who, a, _end in SCRIPT:
        if who == "S":
            phase |= (t - 0.12 >= a) & (t - 0.12 < a + 3.0)
    away = phase | (talking & ((t % 7.0) < 1.2))
    g.yaw[:] = np.where(away, 22.0, 25.0) + rng.normal(0, 0.8, len(t))
    g.pitch[:] = np.where(away, 18.0, -5.0) + rng.normal(0, 0.8, len(t))
    blinks = np.arange(3.0, DURATION_S, 4.3)
    for b in blinks:
        g.yaw[(t >= b) & (t < b + 0.12)] = np.nan
    g.pitch[np.isnan(g.yaw)] = np.nan
    g.face[:] = True
    g.origin[:] = [30.0, -40.0, 700.0]
    g.direction[:] = np.nan
    times_ms = (VIDEO_START_NS + t * 1e9) / 1e6
    conv = rec.load_conversation(root, Path(f"video_{CAM}.mp4"))
    args = ap.Namespace(
        target="auto", target_yaw=0.0, target_pitch=0.0, radius=None, min_confidence=0.5
    )
    a = rec.analyse(g, times_ms, conv, args)
    settings = {
        "min_confidence": 0.5,
        "target": "auto",
        "intrinsics": "nominal",
        "conversation": True,
    }
    rec.write_outputs(
        root / "eye_contact", Path(f"video_{CAM}.mp4"), CAM, FPS, times_ms, g, a, settings, None
    )


# ── Heart rate and HRV ───────────────────────────────────────────────────────


def heart_rate(root: Path, rng) -> None:
    import run_rppg
    from rppg.hr_estimation import median_smooth

    t = frame_times_s()
    beats, bt = [], 0.5
    while bt < DURATION_S:
        beats.append(bt)
        bt += 0.8 + 0.04 * math.sin(2 * math.pi * 0.25 * bt) + rng.normal(0, 0.015)
    pulse_dir = np.array([0.33, 0.77, 0.53])
    wave = np.zeros(len(t))
    for b in beats:
        dt = t - b
        wave += np.exp(-0.5 * (dt / 0.08) ** 2) + 0.3 * np.exp(-0.5 * ((dt - 0.3) / 0.06) ** 2)
    base = np.array([150.0, 110.0, 90.0])
    clean = base * (1.0 + 0.004 * wave[:, None] * pulse_dir)
    left = clean + rng.normal(0, 0.085, clean.shape)
    right = clean + rng.normal(0, 0.085, clean.shape)
    whole = (left + right) / 2.0
    ts_ms = list((VIDEO_START_NS + t * 1e9) / 1e6)
    windows = run_rppg._compute_windows(
        ts_ms, [tuple(x) for x in whole], ts_ms, FPS, "pos", 10.0, 2.0
    )
    raw = np.array([w["bpm"] if w["bpm"] is not None else np.nan for w in windows])
    for w, s in zip(windows, median_smooth(raw, 3), strict=False):
        w["smoothed_bpm"] = None if np.isnan(s) else round(float(s), 1)
    halves = [(tuple(a), tuple(b)) for a, b in zip(left, right, strict=True)]
    frames = [
        {
            "frame_index": i,
            "timestamp_ms": round(x),
            "face_detected": True,
            "roi_bbox_px": [560, 330, 140, 90],
        }
        for i, x in enumerate(ts_ms)
    ]
    beat_doc = run_rppg._beats_and_hrv(
        Path(f"video_{CAM}.mp4"),
        ts_ms,
        [tuple(x) for x in whole],
        halves,
        ts_ms,
        FPS,
        root / "rppg",
    )
    run_rppg._write_results(
        Path(f"video_{CAM}.mp4"), windows, frames, "pos", 10.0, 2.0, CAM, root / "rppg", beat_doc
    )


# ── 2D Gaze and the report ───────────────────────────────────────────────────


def gaze2d(root: Path) -> None:
    import run_gaze2d

    t = frame_times_s()
    talking = subject_speaking(t - 0.12)
    frames, on, dx_sum, dy_sum = [], 0, 0.0, 0.0
    for i, ti in enumerate(t):
        dx, dy = (0.35, -0.3) if talking[i] and (ti % 7.0) < 2.0 else (0.05, 0.02)
        frames.append(
            {
                "frame_index": i,
                "timestamp_ms": round((VIDEO_START_NS + ti * 1e9) / 1e6),
                "face_detected": True,
                "face_box_px": [500, 200, 760, 520],
                "gaze_dx": dx,
                "gaze_dy": dy,
            }
        )
        on += abs(dx) < 0.2 and abs(dy) < 0.2
        dx_sum += dx
        dy_sum += dy
    n = len(frames)
    summary = {
        "pct_frames_with_face": 1.0,
        "mean_gaze_dx": round(dx_sum / n, 4),
        "mean_gaze_dy": round(dy_sum / n, 4),
        "pct_on_target": round(on / n, 3),
    }
    run_gaze2d._write_results(
        Path(f"video_{CAM}.mp4"), frames, summary, CAM, 0.5, output_dir=root / "gaze2d"
    )


def report(root: Path) -> None:
    import run_session_report

    run_session_report.report_session(root, html=False)


# ── Build ────────────────────────────────────────────────────────────────────


def build(dest: Path) -> Path:
    """Build the golden session into ``dest`` (which must not exist) and
    keep only the :data:`KEEP` files."""
    # The plugins print non-ASCII progress text; a Windows console is cp1252.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    rng = np.random.default_rng(20261010)
    work = Path(tempfile.mkdtemp(prefix="golden_"))
    try:
        session = work / "session"
        write_session(session)
        write_audio(session, rng)
        face_dynamics(session)
        conversation(session)
        eye_contact(session, rng)
        heart_rate(session, rng)
        gaze2d(session)
        report(session)
        dest.mkdir(parents=True)
        for rel in KEEP:
            target = dest / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(session / rel, target)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return dest


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="Build the golden analysis session")
    p.add_argument("--update", action="store_true", help=f"Replace the committed copy in {GOLDEN}")
    p.add_argument("--out", type=Path, help="Build into this (new) folder instead")
    args = p.parse_args(argv)
    if args.update:
        if GOLDEN.exists():
            shutil.rmtree(GOLDEN)
        build(GOLDEN)
        print(f"Golden session updated: {GOLDEN}")
        return 0
    if args.out:
        build(args.out)
        print(f"Golden session built: {args.out}")
        return 0
    p.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
