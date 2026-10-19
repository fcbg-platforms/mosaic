"""
Read what the analysis plugins wrote for one session.

Every reader returns ``None`` when its plugin has not been run (or its file
cannot be read), so a report works for any mix of analyses. Readers keep the
parts a report needs: summaries, events and coarse time series; never
per-frame landmarks.

Times are kept as the plugins wrote them (``elapsed_ms``, the clock of
``video/timestamps_camN.csv``) and turned into seconds from each video's
first frame by :func:`video_start_ms`.
"""

from __future__ import annotations

import csv
import json
import re
from dataclasses import dataclass, field
from pathlib import Path

#: Analyses a report knows about, for the "not run yet" list.
PER_CAMERA_ANALYSES = (
    ("face_dynamics", "Face Dynamics"),
    ("conversation", "Conversation Timing"),
    ("eye_contact", "Eye Contact"),
    ("rppg", "Heart rate and HRV"),
    ("gaze2d", "2D Gaze"),
    ("expression", "Facial Expression"),
    ("pose", "Pose"),
)
#: A pose track counts as a person when present in this share of frames.
MIN_TRACK_SHARE = 0.05

SESSION_ANALYSES = (
    ("transcripts", "Speaker Diarization"),
    ("voice", "Voice analysis"),
    ("gaze_fusion", "3D Gaze"),
    ("pose3d", "3D Pose Reconstruction"),
    ("sync_repair", "Frame Sync Repair"),
)


def read_json(path: Path) -> dict | None:
    """A JSON object from ``path``, or ``None`` if missing or unreadable."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def camera_index(stem: str) -> int | None:
    m = re.search(r"video_(\d+)", stem)
    return int(m.group(1)) if m else None


def video_span_ms(session: Path, cam: int) -> tuple[float, float] | None:
    """Elapsed times (ms) of a camera's first and last recorded frames."""
    path = session / "video" / f"timestamps_cam{cam}.csv"
    first = last = None
    try:
        with path.open(newline="") as f:
            for row in csv.DictReader(f):
                try:
                    t = int(row["elapsed_ns"]) / 1e6
                except (KeyError, TypeError, ValueError):
                    continue  # a cut-off last row of a recording that crashed
                first = t if first is None else first
                last = t
    except (OSError, ValueError):
        return None
    return None if first is None else (first, last)


@dataclass
class CameraResults:
    """Everything known about one camera's video."""

    index: int
    name: str  # "Camera N", as in the app
    model: str  # the camera model from session_meta.json, may be empty
    t0_ms: float | None  # first frame, elapsed ms
    duration_s: float | None
    results: dict = field(default_factory=dict)
    #: Still frames for the report, (seconds into the video, JPEG bytes);
    #: only when asked for (they show the participants).
    stills: list = field(default_factory=list)


@dataclass
class SessionData:
    path: Path
    meta: dict
    notes: str
    cameras: list[CameraResults]
    session_results: dict


# ── Per-camera readers ───────────────────────────────────────────────────────


def _face_dynamics(session: Path, stem: str) -> dict | None:
    d = read_json(session / "face_dynamics" / f"{stem}.face_dynamics.json")
    if d is None or d.get("schema") != "mosaic-face-dynamics-v1":
        return None
    return {"summary": d.get("summary", {}), "events": d.get("events", []), "fps": d.get("fps")}


def _conversation(session: Path, stem: str) -> dict | None:
    d = read_json(session / "conversation" / f"{stem}.conversation.json")
    if d is None or d.get("schema") != "mosaic-conversation-v1":
        return None
    return {
        "summary": d.get("summary", {}),
        "spurts": d.get("spurts", []),
        "transitions": d.get("transitions", []),
        "turns": d.get("turns", []),
        "timing": d.get("timing", {}),
        "attribution": d.get("attribution", {}),
    }


def _eye_contact(session: Path, stem: str) -> dict | None:
    d = read_json(session / "eye_contact" / f"{stem}.eye_contact.json")
    if d is None or d.get("schema") != "mosaic-eye-contact-v1":
        return None
    # Runs of contact (1) and looking away (0), instead of every frame.
    runs, prev, start = [], None, None
    for f in d.get("frames", []):
        c = f.get("contact")
        state = None if c is None else bool(c)
        if state != prev:
            if prev is not None:
                runs.append((start, f["timestamp_ms"], prev))
            prev, start = state, f["timestamp_ms"]
    if prev is not None and d.get("frames"):
        runs.append((start, d["frames"][-1]["timestamp_ms"], prev))
    return {
        "summary": d.get("summary", {}),
        "target": d.get("target", {}),
        "aversions": d.get("aversions", []),
        "runs": runs,
    }


def _rppg(session: Path, stem: str) -> dict | None:
    # One file per backend; the most recently written one is reported.
    files = sorted((session / "rppg").glob(f"{stem}.*.rppg.json"), key=lambda p: p.stat().st_mtime)
    if not files:
        return None
    d = read_json(files[-1])
    if d is None:
        return None
    return {
        "backend": d.get("backend"),
        "summary": d.get("summary", {}),
        "windows": d.get("windows", []),
        "hrv": d.get("hrv"),
        "hrv_withheld": d.get("hrv_withheld", []),
        "hrv_windows": d.get("hrv_windows", []),
        "by_state": d.get("by_state"),
    }


def _gaze2d(session: Path, stem: str) -> dict | None:
    d = read_json(session / "gaze2d" / f"{stem}.gaze2d.json")
    return None if d is None else {"summary": d.get("summary", {})}


def _expression(session: Path, stem: str) -> dict | None:
    d = read_json(session / "expression" / f"{stem}.expression.json")
    if d is None:
        return None
    counts: dict = {}
    frames = d.get("frames", [])
    for f in frames:
        subjects = f.get("subjects") or []
        if subjects:
            label = subjects[0].get("dominant_expression")
            if label:
                counts[label] = counts.get(label, 0) + 1
    total = sum(counts.values())
    shares = {
        k: round(100.0 * v / total, 1) for k, v in sorted(counts.items(), key=lambda kv: -kv[1])
    }
    return {
        "backend": d.get("backend"),
        "frames_with_face": total,
        "frames": len(frames),
        "shares_pct": shares,
    }


def _pose(session: Path, stem: str) -> dict | None:
    files = sorted((session / "pose").glob(f"{stem}.*.pose.json"), key=lambda p: p.stat().st_mtime)
    if not files:
        return None
    d = read_json(files[-1])
    if d is None:
        return None
    frames = d.get("frames", [])
    seen: dict = {}
    with_person = 0
    for f in frames:
        people = f.get("subjects") or []
        if people:
            with_person += 1
        for p in people:
            sid = p.get("subject_id") if isinstance(p, dict) else None
            # Untracked detections have ids below 1 (-1, -2, ...).
            if isinstance(sid, int) and sid >= 1:
                seen[sid] = seen.get(sid, 0) + 1
    # A person is a track present in at least this share of frames; tracker
    # id switches leave many brief tracks that are not more people.
    steady = [k for k, n in seen.items() if frames and n >= MIN_TRACK_SHARE * len(frames)]
    return {
        "model": d.get("model"),
        "frames": len(frames),
        "frames_with_person_pct": round(100.0 * with_person / len(frames), 1) if frames else None,
        "tracked_people": len(steady) if d.get("tracker") else None,
    }


#: Outputs the summary row does not use, skipped by summary-only runs.
NOT_IN_SUMMARY = {"pose"}

PER_CAMERA_READERS = {
    "face_dynamics": _face_dynamics,
    "conversation": _conversation,
    "eye_contact": _eye_contact,
    "rppg": _rppg,
    "gaze2d": _gaze2d,
    "expression": _expression,
    "pose": _pose,
}


# ── Session-level readers ────────────────────────────────────────────────────


def _transcripts(session: Path) -> list | None:
    out = []
    for path in sorted((session / "audio").glob("*.transcript.json")):
        d = read_json(path)
        if d is None:
            continue
        segs = d.get("segments", [])
        speakers: dict = {}
        for s in segs:
            who = s.get("speaker") or "unlabelled"
            words = len((s.get("text") or "").split())
            dur = max(0.0, (s.get("end_ms", 0) - s.get("start_ms", 0)) / 1000.0)
            acc = speakers.setdefault(who, {"segments": 0, "words": 0, "seconds": 0.0})
            acc["segments"] += 1
            acc["words"] += words
            acc["seconds"] += dur
        out.append(
            {
                "audio": d.get("source_audio", path.name),
                "language": d.get("language"),
                "diarization": d.get("diarization_status")
                or ("ok" if d.get("diarization") else None),
                "segments": len(segs),
                "words": sum(v["words"] for v in speakers.values()),
                "speakers": {
                    k: {**v, "seconds": round(v["seconds"], 1)} for k, v in speakers.items()
                },
                "lines": [
                    (s.get("start_ms", 0), s.get("speaker") or "", (s.get("text") or "").strip())
                    for s in segs
                ],
            }
        )
    return out or None


def _voice(session: Path) -> list | None:
    out = []
    for path in sorted((session / "audio").glob("*.voice.json")):
        d = read_json(path)
        if d is None:
            continue
        pitch = [v for v in d.get("pitch", {}).get("values_hz", []) if v and v > 0]
        loud = d.get("intensity", {}).get("values_db", [])
        pitch.sort()
        out.append(
            {
                "audio": d.get("source_audio", path.name),
                "duration_s": round(d.get("duration_ms", 0) / 1000.0, 1),
                "median_pitch_hz": round(pitch[len(pitch) // 2], 1) if pitch else None,
                "voiced_pct": round(
                    100.0 * len(pitch) / max(1, len(d.get("pitch", {}).get("values_hz", []))), 1
                ),
                "mean_intensity_db": round(sum(loud) / len(loud), 1) if loud else None,
                # The spectrogram picture run_voice.py draws next to its JSON.
                "image": _existing(path.with_name(path.name.replace(".voice.json", ".voice.png"))),
                "spectrogram": d.get("spectrogram") or {},
                "pitch": d.get("pitch") or {},
            }
        )
    return out or None


def _existing(path: Path) -> Path | None:
    return path if path.is_file() else None


def _gaze_fusion(session: Path) -> dict | None:
    s = read_json(session / "gaze_fusion" / "summary.json")
    return None if s is None else {"summary": s}


def _pose3d(session: Path) -> dict | None:
    """The quality block of ``skeleton3d.json``, without reading the rest:
    the file holds every keypoint of every tick and can be hundreds of MB.
    run_pose3d.py writes ``quality`` just before ``frames``, so the text up to
    ``"frames"`` is a JSON object once closed."""
    path = session / "skeleton3d.json"
    head = ""
    try:
        with path.open(encoding="utf-8") as f:
            while '"frames"' not in head:
                chunk = f.read(1 << 16)
                if not chunk:
                    break
                head += chunk
    except OSError:
        return None
    cut = head.find('"frames"')
    if cut < 0:
        return None
    try:
        d = json.loads(head[:cut].rstrip().rstrip(",") + "}")
    except ValueError:
        return None
    return {"quality": d.get("quality") if isinstance(d, dict) else None}


def _sync_repair(session: Path) -> dict | None:
    d = read_json(session / "synced" / "sync_repair.json")
    if d is None:
        return None
    cams = d.get("cameras", [])
    return {
        "alignment": d.get("alignment"),
        "fps": d.get("master_fps"),
        "duration_ms": d.get("duration_ms"),
        "cameras": cams,
    }


SESSION_READERS = {
    "transcripts": _transcripts,
    "voice": _voice,
    "gaze_fusion": _gaze_fusion,
    "pose3d": _pose3d,
    "sync_repair": _sync_repair,
}

#: Session outputs the summary row does not use (large files).
SESSION_NOT_IN_SUMMARY = {"pose3d"}


def still_frames(video: Path, count: int = 3, width: int = 360) -> list:
    """``count`` frames spread over the video, scaled to ``width`` pixels,
    as (seconds, JPEG bytes). Empty when the video cannot be read."""
    try:
        import cv2
    except ImportError:
        return []
    cap = cv2.VideoCapture(str(video))
    try:
        n = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
        if not cap.isOpened() or n <= 0:
            return []  # unreadable, or a container that does not say its length
        out = []
        for k in range(count):
            idx = int((k + 0.5) / count * n)
            cap.set(cv2.CAP_PROP_POS_FRAMES, idx)
            ok, img = cap.read()
            if not ok:
                continue
            h, w = img.shape[:2]
            img = cv2.resize(
                img, (width, max(1, round(h * width / w))), interpolation=cv2.INTER_AREA
            )
            ok, buf = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, 80])
            if ok:
                out.append((idx / fps, buf.tobytes()))
        return out
    finally:
        cap.release()


# ── The session ──────────────────────────────────────────────────────────────


def collect(session: Path, for_summary_only: bool = False, stills: bool = False) -> SessionData:
    """Read everything there is about one session.

    ``for_summary_only`` skips outputs the summary row does not use (pose,
    whose files can be hundreds of megabytes). ``stills`` also takes a few
    still frames from each video (they show the participants, so only when
    asked for).
    """
    session = Path(session)
    meta = read_json(session / "session_meta.json") or {}
    try:
        # errors="replace": notes edited outside the app may be cp1252.
        notes = (session / "notes.txt").read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        notes = ""
    models = {int(c.get("index", -1)): c.get("name") or "" for c in meta.get("cameras", [])}
    cameras = []
    for video in sorted((session / "video").glob("video_*.mp4")):
        idx = camera_index(video.stem)
        if idx is None:
            continue
        span = video_span_ms(session, idx)
        cam = CameraResults(
            idx,
            f"Camera {idx + 1}",
            models.get(idx, ""),
            span[0] if span else None,
            (span[1] - span[0]) / 1000.0 if span else None,
        )
        for key, reader in PER_CAMERA_READERS.items():
            if for_summary_only and key in NOT_IN_SUMMARY:
                continue
            r = reader(session, video.stem)
            if r is not None:
                cam.results[key] = r
        if stills and not for_summary_only:
            cam.stills = still_frames(video)
        cameras.append(cam)
    results = {}
    for key, reader in SESSION_READERS.items():
        if for_summary_only and key in SESSION_NOT_IN_SUMMARY:
            continue
        r = reader(session)
        if r is not None:
            results[key] = r
    return SessionData(session, meta, notes, cameras, results)


def duration_s(data: SessionData) -> float | None:
    """The recording's length: from its end record, else the frame sync,
    else the longest camera's timestamps."""
    end = data.meta.get("session_end")
    if isinstance(end, dict) and end.get("duration_ms"):
        return round(end["duration_ms"] / 1000.0, 1)
    sync = data.session_results.get("sync_repair")
    if sync and sync.get("duration_ms"):
        return round(sync["duration_ms"] / 1000.0, 1)
    spans = [c.duration_s for c in data.cameras if c.duration_s is not None]
    return round(max(spans), 1) if spans else None
