"""
MOSAIC Face Dynamics: blinks, expression events, expressivity and head nods
and shakes from each camera's video. Built for interview mode (one camera,
a large face, about 50 fps); see :mod:`face_dynamics`.

    python analysis/run_face_dynamics.py --session /path/to/session
    python analysis/run_face_dynamics.py --video /path/to/video/video_0.mp4

Outputs, in ``<session>/face_dynamics/`` (``--video``: the session's folder
when the video is in a session's ``video/`` folder, else beside the video):

* ``<stem>.face_dynamics.json``: per-frame signals, events and a summary.
* ``<stem>.face_dynamics.csv``: per-frame signals and all 52 blendshapes.
* ``<stem>.events.csv``: one row per event.
* ``<stem>.face_dynamics.mp4``: the video with eyes, lips, head direction,
  the current events and running counts drawn on it (``--no-video``: none).

There is no frame-skip option: a blink lasts 5 to 15 frames at 50 fps, and
skipping frames would miss short ones and blur every duration.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
import time
import traceback
from pathlib import Path

import numpy as np

TAG = "[run_face_dynamics]"
SCHEMA = "mosaic-face-dynamics-v1"
#: Below this share of frames with a face, the run says why the results are thin.
LOW_FACE_SHARE = 0.5
#: Below this face width (px) eyelid and head-pose noise swamps blinks and nods.
SMALL_FACE_PX = 100


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="MOSAIC face dynamics (blinks, expressions, head gestures)"
    )
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument("--session", metavar="DIR", help="Process the videos of a recorded session")
    group.add_argument("--video", metavar="FILE", help="Process one video file")
    p.add_argument("--camera", type=int, default=None, help="Only this camera index (--session)")
    p.add_argument(
        "--min-confidence",
        type=float,
        default=0.5,
        help="Face detection/tracking threshold (default 0.5)",
    )
    p.add_argument("--no-video", action="store_true", help="Do not write the annotated video")
    return p.parse_args(argv)


def log(msg: str) -> None:
    print(f"{TAG} {msg}", flush=True)


# ── Inputs ───────────────────────────────────────────────────────────────────


def camera_index_from_filename(video_path: Path) -> int:
    m = re.search(r"video_(\d+)", video_path.stem)
    if not m:
        log(f"Warning: no camera index in {video_path.name}; assuming 0")
        return 0
    return int(m.group(1))


def read_timestamps_ms(video_path: Path, camera_index: int) -> list[float]:
    """``elapsed_ns`` of each frame from ``timestamps_camN.csv``, in ms."""
    path = video_path.with_name(f"timestamps_cam{camera_index}.csv")
    out: list[float] = []
    if path.exists():
        with path.open(newline="") as f:
            for row in csv.DictReader(f):
                try:
                    out.append(int(row.get("elapsed_ns", 0)) / 1e6)
                except ValueError:
                    out.append(float("nan"))
    return out


def frame_times_ms(stamps: list[float], n: int, fps: float) -> np.ndarray:
    """One time per decoded frame: the recorded timestamps where present,
    continued at the video's frame rate where they run out or are bad."""
    t = np.full(n, np.nan)
    m = min(n, len(stamps))
    t[:m] = stamps[:m]
    step = 1000.0 / fps
    if not np.isfinite(t).any():
        return np.arange(n) * step
    for i in range(n):
        if not np.isfinite(t[i]) or (i and t[i] <= t[i - 1]):
            t[i] = (t[i - 1] + step) if i else (t[np.isfinite(t)][0] - step)
    return t


def adjust_for_crop(k: np.ndarray, cal: dict, cam: dict) -> np.ndarray:
    """Shift the principal point when the recording used another sensor
    region than the calibration (same rule as the 3D gaze plugin)."""
    rec_x, rec_y = cam.get("offset_x"), cam.get("offset_y")
    cal_x, cal_y = cal.get("offset_x"), cal.get("offset_y")
    if rec_x is None or rec_y is None:
        return k
    if cal_x is None or cal_y is None or cal_x < 0 or cal_y < 0:
        cal_x = cal_y = 0
    out = k.copy()
    out[0, 2] -= float(rec_x) - float(cal_x)
    out[1, 2] -= float(rec_y) - float(cal_y)
    return out


def camera_intrinsics(session: Path | None, camera_index: int, width: int, height: int):
    """``(K, dist, calibrated)`` from ``session_meta.json``, or a nominal
    camera when the session has no calibration for this camera."""
    from face_dynamics.extract import nominal_camera

    if session is not None:
        try:
            meta = json.loads((session / "session_meta.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            meta = {}
        for cam in meta.get("cameras", []):
            if int(cam.get("index", -1)) != camera_index:
                continue
            cal = cam.get("calibration") or {}
            if cal.get("calibrated") and cal.get("camera_matrix") and cal.get("dist_coeffs"):
                k = np.array(cal["camera_matrix"], dtype=np.float64).reshape(3, 3)
                return (
                    adjust_for_crop(k, cal, cam),
                    np.array(cal["dist_coeffs"], dtype=np.float64),
                    True,
                )
    k, d = nominal_camera(width, height)
    return k, d, False


# ── Measurement ──────────────────────────────────────────────────────────────


class Measurements:
    """Per-frame measurements of one video (``nan`` where no face)."""

    def __init__(self, n: int, n_outline: int) -> None:
        self.found = np.zeros(n, bool)
        self.ear = np.full((n, 2), np.nan)  # right, left
        self.angles = np.full((n, 3), np.nan)  # yaw, pitch, roll
        self.box = np.full((n, 4), np.nan)
        self.nose = np.full((n, 2), np.nan)
        self.outline = np.full((n, n_outline, 2), np.nan, dtype=np.float32)
        self.shapes: dict[str, np.ndarray] = {}
        self.n = n

    def put(self, i: int, face) -> None:
        self.found[i] = True
        self.ear[i] = (face.ear_right, face.ear_left)
        self.angles[i] = (face.yaw, face.pitch, face.roll)
        self.box[i] = face.box
        self.nose[i] = face.nose
        self.outline[i] = face.outline
        for name, score in face.blendshapes.items():
            if name not in self.shapes:
                self.shapes[name] = np.full(self.n, np.nan)
            self.shapes[name][i] = score

    def trim(self, n: int) -> None:
        """Keep the first ``n`` frames (the video was shorter than its header said)."""
        self.n = n
        for name in ("found", "ear", "angles", "box", "nose", "outline"):
            setattr(self, name, getattr(self, name)[:n])
        self.shapes = {k: v[:n] for k, v in self.shapes.items()}


def measure_video(cap, total: int, tracker, progress) -> Measurements:
    from face_dynamics.extract import EYE_OUTLINE_LEFT, EYE_OUTLINE_RIGHT, LIPS_OUTER

    m = Measurements(total, len(EYE_OUTLINE_RIGHT) + len(EYE_OUTLINE_LEFT) + len(LIPS_OUTER))
    i = 0
    while i < total:
        ok, frame = cap.read()
        if not ok:
            break
        face = tracker.measure(frame)
        if face is not None:
            m.put(i, face)
        i += 1
        progress(i)
    if i < total:
        if i < total - 1:  # one frame short of the header's count is common
            log(f"The video ended after {i} of {total} frames.")
        m.trim(i)
    return m


# ── Analysis ─────────────────────────────────────────────────────────────────


def analyse(m: Measurements, times_s: np.ndarray) -> dict:
    """Signals, events and summary from the measurements."""
    from face_dynamics import metrics as fm

    shapes = {name: m.shapes.get(name, np.full(m.n, np.nan)) for name in _needed_shapes()}
    open_r = fm.openness(m.ear[:, 0], times_s)
    open_l = fm.openness(m.ear[:, 1], times_s)
    yaw, pitch, roll = (fm.smooth(m.angles[:, k], times_s) for k in range(3))
    speed = fm.angular_speed(yaw, pitch, roll, times_s)
    expr = fm.expressivity(shapes, m.found)
    events = (
        fm.detect_blinks(open_l, open_r, times_s)
        + fm.detect_expression_events(shapes, times_s)
        + fm.detect_head_gestures(yaw, pitch, times_s)
    )
    events.sort(key=lambda e: (e.start_s, e.kind))
    summary = fm.summarise(events, times_s, m.found, open_l, open_r, expr, speed)
    return {
        "open_left": open_l,
        "open_right": open_r,
        "yaw": yaw,
        "pitch": pitch,
        "roll": roll,
        "speed": speed,
        "expressivity": expr,
        "smile": fm.channel(shapes, "mouthSmileLeft", "mouthSmileRight"),
        "brow": (shapes["browInnerUp"] + fm.channel(shapes, "browOuterUpLeft", "browOuterUpRight"))
        / 2.0,
        "events": events,
        "summary": summary,
    }


def _needed_shapes() -> list[str]:
    from face_dynamics.metrics import EXPRESSIVE

    return sorted(set(EXPRESSIVE) | {"browInnerUp", "browOuterUpLeft", "browOuterUpRight"})


# ── Outputs ──────────────────────────────────────────────────────────────────


def _num(x, nd: int = 3):
    """JSON number, ``None`` for nan."""
    x = float(x)
    return None if not math.isfinite(x) else round(x, nd)


def write_outputs(
    out_dir: Path,
    video_path: Path,
    camera_index: int,
    fps: float,
    times_ms: np.ndarray,
    m: Measurements,
    a: dict,
    settings: dict,
    video_rel: str | None,
) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = video_path.stem
    t0 = float(times_ms[0]) if len(times_ms) else 0.0
    frames = []
    for i in range(m.n):
        f = {
            "frame_index": i,
            "timestamp_ms": int(round(times_ms[i])),
            "face_detected": bool(m.found[i]),
        }
        if m.found[i]:
            f.update(
                {
                    "face_box_px": [round(float(v)) for v in m.box[i]],
                    "openness_left": _num(a["open_left"][i]),
                    "openness_right": _num(a["open_right"][i]),
                    "smile": _num(a["smile"][i]),
                    "brow_raise": _num(a["brow"][i]),
                    "expressivity": _num(a["expressivity"][i], 4),
                    "yaw": _num(a["yaw"][i], 2),
                    "pitch": _num(a["pitch"][i], 2),
                    "roll": _num(a["roll"][i], 2),
                    "head_speed": _num(a["speed"][i], 1),
                }
            )
        frames.append(f)

    def ev_json(e):
        return {
            "kind": e.kind,
            "start_ms": int(round(t0 + e.start_s * 1000.0)),
            "end_ms": int(round(t0 + e.end_s * 1000.0)),
            "peak_ms": int(round(t0 + e.peak_s * 1000.0)),
            "duration_ms": round(e.duration_ms, 1),
            "peak": _num(e.peak),
            **e.extra,
        }

    doc = {
        "schema": SCHEMA,
        "source_video": video_path.name,
        "camera_index": camera_index,
        "fps": round(fps, 3),
        "settings": settings,
        "annotated_video": video_rel,
        "summary": a["summary"],
        "events": [ev_json(e) for e in a["events"]],
        "frames": frames,
    }
    json_path = out_dir / f"{stem}.face_dynamics.json"
    json_path.write_text(json.dumps(doc, indent=1), encoding="utf-8")

    names = sorted(m.shapes)
    with (out_dir / f"{stem}.face_dynamics.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(
            [
                "frame_index",
                "timestamp_ms",
                "face_detected",
                "ear_left",
                "ear_right",
                "openness_left",
                "openness_right",
                "yaw",
                "pitch",
                "roll",
                "head_speed",
                "expressivity",
                *names,
            ]
        )
        for i in range(m.n):
            vals = [
                m.ear[i, 1],
                m.ear[i, 0],
                a["open_left"][i],
                a["open_right"][i],
                a["yaw"][i],
                a["pitch"][i],
                a["roll"][i],
                a["speed"][i],
                a["expressivity"][i],
                *(m.shapes[n][i] for n in names),
            ]
            w.writerow(
                [
                    i,
                    int(round(times_ms[i])),
                    int(m.found[i]),
                    *("" if not np.isfinite(v) else f"{v:.4f}" for v in vals),
                ]
            )

    with (out_dir / f"{stem}.events.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["kind", "start_ms", "end_ms", "duration_ms", "peak", "detail"])
        for e in a["events"]:
            j = ev_json(e)
            detail = ";".join(f"{k}={v}" for k, v in e.extra.items())
            w.writerow([e.kind, j["start_ms"], j["end_ms"], j["duration_ms"], j["peak"], detail])
    return json_path


# ── Annotated video ──────────────────────────────────────────────────────────

GREEN, RED, AMBER, CYAN, WHITE = (
    (90, 220, 90),
    (60, 60, 230),
    (40, 190, 240),
    (230, 200, 60),
    (235, 235, 235),
)
LABELS = {
    "blink": "BLINK",
    "long_closure": "EYES CLOSED",
    "smile": "SMILE",
    "duchenne_smile": "DUCHENNE SMILE",
    "brow_raise": "BROW RAISE",
    "nod": "NOD",
    "shake": "HEAD SHAKE",
}


def render_video(
    cap, out_path: Path, fps: float, times_s: np.ndarray, m: Measurements, a: dict, progress
) -> bool:
    """Draw the measurements and events on every frame. Written to a
    ``.partial.mp4`` first, so a failed run leaves no half video behind."""
    import cv2
    from face_dynamics.extract import EYE_OUTLINE_LEFT, EYE_OUTLINE_RIGHT
    from gaze.render import open_writer, partial_path

    n_r, n_l = len(EYE_OUTLINE_RIGHT), len(EYE_OUTLINE_LEFT)
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    partial = partial_path(out_path)
    writer = open_writer(partial, fps, (w, h))
    if writer is None:
        log("Cannot open a video writer; no annotated video.")
        return False
    scale = max(0.5, h / 720.0)
    font = cv2.FONT_HERSHEY_SIMPLEX
    events = a["events"]
    # Running counts: how many of each kind had started by each frame.
    starts = {k: np.array(sorted(e.start_s for e in events if e.kind == k)) for k in LABELS}
    history_s = 4.0

    def text(img, s, org, color, size=0.6, thick=1):
        fg = max(1, int(round(thick * scale)))
        cv2.putText(img, s, org, font, size * scale, (0, 0, 0), fg + 2, cv2.LINE_AA)
        cv2.putText(img, s, org, font, size * scale, color, fg, cv2.LINE_AA)

    ok_all = False
    try:
        for i in range(m.n):
            ok, img = cap.read()
            if not ok:
                break
            t = times_s[i]
            active = [e for e in events if e.start_s <= t < e.end_s]
            closed = any(e.kind in ("blink", "long_closure") for e in active)
            if m.found[i]:
                pts = m.outline[i].astype(np.int32)
                eye_color = RED if closed else GREEN
                cv2.polylines(img, [pts[:n_r]], True, eye_color, max(1, int(scale)), cv2.LINE_AA)
                cv2.polylines(
                    img, [pts[n_r : n_r + n_l]], True, eye_color, max(1, int(scale)), cv2.LINE_AA
                )
                smiling = any(e.kind == "smile" for e in active)
                cv2.polylines(
                    img,
                    [pts[n_r + n_l :]],
                    True,
                    AMBER if smiling else CYAN,
                    max(1, int(scale)),
                    cv2.LINE_AA,
                )
                yaw, pitch = a["yaw"][i], a["pitch"][i]
                if np.isfinite(yaw) and np.isfinite(pitch):
                    x1, y1, x2, y2 = m.box[i]
                    length = 0.6 * (x2 - x1)
                    nx, ny = m.nose[i]
                    tip = (
                        int(nx + length * math.sin(math.radians(yaw))),
                        int(ny - length * math.sin(math.radians(pitch))),
                    )
                    cv2.arrowedLine(
                        img,
                        (int(nx), int(ny)),
                        tip,
                        WHITE,
                        max(1, int(2 * scale)),
                        cv2.LINE_AA,
                        tipLength=0.2,
                    )
            else:
                text(img, "no face", (int(12 * scale), int(h - 16 * scale)), RED)

            # Panel: running counts.
            n_blinks = int(np.searchsorted(starts["blink"], t, side="right"))
            minutes = max(t - times_s[0], 1e-6) / 60.0
            lines = [
                f"blinks {n_blinks}"
                + (f"  ({n_blinks / minutes:.0f}/min)" if minutes > 0.25 else ""),
                f"smiles {int(np.searchsorted(starts['smile'], t, side='right'))}"
                f"  duchenne {int(np.searchsorted(starts['duchenne_smile'], t, side='right'))}",
                f"brow raises {int(np.searchsorted(starts['brow_raise'], t, side='right'))}",
                f"nods {int(np.searchsorted(starts['nod'], t, side='right'))}"
                f"  shakes {int(np.searchsorted(starts['shake'], t, side='right'))}",
            ]
            y = int(28 * scale)
            for line in lines:
                text(img, line, (int(12 * scale), y), WHITE)
                y += int(24 * scale)
            # Current events, newest last, in large type.
            shown = []
            for e in active:
                label = LABELS[e.kind]
                if e.kind == "smile" and any(
                    o.kind == "duchenne_smile" and o.start_s == e.start_s for o in active
                ):
                    continue
                if label not in shown:
                    shown.append(label)
            y = int(28 * scale)
            for label in shown:
                size = cv2.getTextSize(label, font, 0.9 * scale, int(2 * scale))[0]
                text(img, label, (w - size[0] - int(14 * scale), y), AMBER, 0.9, 2)
                y += int(34 * scale)

            # Strip: eye openness over the last few seconds.
            sw, sh = int(260 * scale), int(60 * scale)
            x0, y0 = w - sw - int(12 * scale), h - sh - int(12 * scale)
            strip = img[y0 : y0 + sh, x0 : x0 + sw]
            strip[:] = (strip * 0.55).astype(img.dtype)  # darkened backdrop
            lo = int(np.searchsorted(times_s, t - history_s))
            seg = slice(lo, i + 1)
            both = (a["open_left"][seg] + a["open_right"][seg]) / 2.0
            xs = x0 + (times_s[seg] - (t - history_s)) / history_s * sw
            ys = y0 + sh - np.clip(both, 0.0, 1.2) / 1.2 * sh
            # One line per stretch with a face, so gaps stay gaps.
            good = np.isfinite(ys)
            edges = np.flatnonzero(np.diff(np.concatenate([[0], good.astype(np.int8), [0]])))
            for g0, g1 in zip(edges[::2], edges[1::2], strict=True):
                if g1 - g0 > 1:
                    poly = np.column_stack([xs[g0:g1], ys[g0:g1]]).astype(np.int32)
                    cv2.polylines(img, [poly], False, GREEN, max(1, int(scale)), cv2.LINE_AA)
            text(img, "eye openness", (x0 + int(6 * scale), y0 + int(16 * scale)), WHITE, 0.45)

            writer.write(img)
            progress(i + 1)
        else:
            ok_all = True
    finally:
        writer.release()
        if not ok_all:  # ended early, or an exception is on its way out
            partial.unlink(missing_ok=True)
    if not ok_all:
        log("The video ended early while drawing; no annotated video.")
        return False
    partial.replace(out_path)
    return True


# ── Per video ────────────────────────────────────────────────────────────────


def process_video(
    video_path: Path,
    camera_index: int,
    out_dir: Path,
    session: Path | None,
    min_confidence: float,
    draw: bool,
) -> bool:
    import cv2
    from face_dynamics.extract import FaceTracker

    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        log(f"Cannot open {video_path}")
        return False
    fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    if total <= 0:
        log(f"{video_path.name}: no frames")
        return False
    log(f"Processing {video_path.name}  ({total} frames @ {fps:.1f} fps)")
    if fps < 30.0:
        log(
            f"{fps:.0f} fps is low for blinks (100 to 300 ms each): durations will be "
            "coarse and short blinks missed."
        )

    k, d, calibrated = camera_intrinsics(session, camera_index, width, height)
    if not calibrated:
        log(
            "No intrinsic calibration for this camera: head angles use a nominal lens "
            "(close, not exact)."
        )
    times_ms = frame_times_ms(read_timestamps_ms(video_path, camera_index), total, fps)

    passes = 2 if draw else 1
    every = max(1, total * passes // 200)
    t_start = time.perf_counter()

    def progress_for(pass_index):
        def report(done):
            step = pass_index * total + done
            if step % every == 0 or step == passes * total:
                pct = 100.0 * step / (passes * total)
                elapsed = time.perf_counter() - t_start
                print(
                    f"  {pct:5.1f}%  ({step}/{passes * total})  {elapsed:.1f}s elapsed", flush=True
                )

        return report

    tracker = FaceTracker(k, d, min_confidence=min_confidence)
    try:
        m = measure_video(cap, total, tracker, progress_for(0))
    finally:
        tracker.close()
        cap.release()
    if m.n == 0:
        log(f"{video_path.name}: no frames could be read")
        return False
    times_ms = times_ms[: m.n]
    times_s = (times_ms - times_ms[0]) / 1000.0
    share = float(m.found.mean())
    if share < LOW_FACE_SHARE:
        log(
            f"A face was found in only {100 * share:.0f}% of frames "
            "(it must be visible and not turned far away)."
        )
    if m.found.any():
        width = float(np.median(m.box[m.found, 2] - m.box[m.found, 0]))
        if width < SMALL_FACE_PX:
            log(
                f"The face is about {width:.0f} px wide. Blinks and head gestures need about "
                f"{SMALL_FACE_PX} px or more (interview mode); treat these events as rough."
            )

    a = analyse(m, times_s)
    video_rel = None
    out_video = out_dir / f"{video_path.stem}.face_dynamics.mp4"
    if draw and m.found.any():
        out_dir.mkdir(parents=True, exist_ok=True)
        cap = cv2.VideoCapture(str(video_path))
        try:
            if render_video(cap, out_video, fps, times_s, m, a, progress_for(1)):
                video_rel = f"{out_dir.name}/{out_video.name}"
        finally:
            cap.release()
    if video_rel is None and out_video.exists():
        out_video.unlink()  # from an earlier run; it would not match these results

    settings = {
        "min_confidence": min_confidence,
        "intrinsics": "calibrated" if calibrated else "nominal",
    }
    json_path = write_outputs(
        out_dir, video_path, camera_index, fps, times_ms, m, a, settings, video_rel
    )
    s = a["summary"]
    log(
        f"Done in {time.perf_counter() - t_start:.1f}s: face {s['face_seen_pct']}% of frames, "
        f"{s['blinks']['count']} blinks, {s['expression']['smiles']} smiles, "
        f"{s['head']['nods']} nods, {s['head']['shakes']} shakes"
    )
    # The bar ends at 100% even when the video was shorter than its header
    # said or nothing was drawn.
    print(f"  100.0%  ({passes * total}/{passes * total})", flush=True)
    log(f"Results -> {json_path}")
    return True


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.video:
        video = Path(args.video)
        if not video.exists():
            log(f"No such video: {video}")
            return 1
        session = video.parent.parent if video.parent.name == "video" else None
        out_dir = (session or video.parent) / "face_dynamics"
        ok = process_video(
            video,
            camera_index_from_filename(video),
            out_dir,
            session,
            args.min_confidence,
            not args.no_video,
        )
        return 0 if ok else 1

    session = Path(args.session)
    videos = sorted((session / "video").glob("video_*.mp4"))
    if args.camera is not None:
        videos = [v for v in videos if camera_index_from_filename(v) == args.camera]
    if not videos:
        print(f"{TAG} No videos to analyse in {session / 'video'}", file=sys.stderr, flush=True)
        return 1
    failures = 0
    for pos, video in enumerate(videos, start=1):
        print(f"{TAG} Camera {pos}/{len(videos)}: {video.name}", flush=True)
        try:
            ok = process_video(
                video,
                camera_index_from_filename(video),
                session / "face_dynamics",
                session,
                args.min_confidence,
                not args.no_video,
            )
        except Exception:  # one camera's failure must not stop the others
            traceback.print_exc()
            log(f"{video.name}: failed (see the error above)")
            ok = False
        failures += not ok
    return 1 if failures == len(videos) else 0


if __name__ == "__main__":
    sys.exit(main())
