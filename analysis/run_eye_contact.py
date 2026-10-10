"""
MOSAIC Eye Contact: where the face on a camera looks, how much of the time it
looks at its conversation partner, and when and where it looks away. Built
for interview mode; see :mod:`eye_contact`.

    python analysis/run_eye_contact.py --session /path/to/session
    python analysis/run_eye_contact.py --session DIR --target camera
    python analysis/run_eye_contact.py --session DIR --target manual
        --target-yaw 25 --target-pitch -5

The partner's direction is found from the data by default (``--target
auto``): where the gaze clusters while the subject listens. With Conversation
Timing's output present (``conversation/video_N.conversation.json``) eye
contact is also split by speaking and listening and lined up with the
subject's turns.

Outputs, in ``<session>/eye_contact/`` per camera:

* ``video_N.eye_contact.json``: the partner's direction and contact cone,
  the summary, every look-away, turn patterns and per-frame gaze.
* ``video_N.eye_contact.csv`` (per frame) and ``video_N.aversions.csv``.
* ``video_N.eye_contact.mp4``: the video with the gaze arrow, the contact
  state and a gaze map around the partner (``--no-video``: none).
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
import time
import traceback
from pathlib import Path

import numpy as np

TAG = "[run_eye_contact]"
SCHEMA = "mosaic-eye-contact-v1"
#: Listening time needed to find the partner from listening alone, seconds.
MIN_LISTENING_S = 20.0
#: Below this share of frames inside the cone, the partner's direction is
#: not clear (no cluster): the log says so.
MIN_TARGET_SHARE = 0.25
#: Contact cone when the partner is the camera or given by hand, degrees:
#: about a face's width at conversation distance plus gaze noise.
FIXED_RADIUS_DEG = 8.0


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="MOSAIC eye contact and gaze aversion")
    p.add_argument("--session", metavar="DIR", required=True, help="Recorded session directory")
    p.add_argument("--camera", type=int, default=None, help="Only this camera index")
    p.add_argument("--min-confidence", type=float, default=0.5, help="Face detection threshold")
    p.add_argument(
        "--target",
        choices=["auto", "camera", "manual"],
        default="auto",
        help="Where the partner is: found from the gaze (auto), the camera, or given",
    )
    p.add_argument("--target-yaw", type=float, default=0.0, help="Manual target yaw, degrees")
    p.add_argument("--target-pitch", type=float, default=0.0, help="Manual target pitch, degrees")
    p.add_argument("--radius", type=float, default=None, help="Contact cone radius, degrees")
    p.add_argument("--no-video", action="store_true", help="Do not write the annotated video")
    return p.parse_args(argv)


def log(msg: str) -> None:
    print(f"{TAG} {msg}", flush=True)


def progress_line(done: int, total: int, t_start: float) -> None:
    pct = 100.0 * done / max(total, 1)
    print(
        f"  {pct:5.1f}%  ({done}/{total})  {time.perf_counter() - t_start:.1f}s elapsed", flush=True
    )


# ── Measurement ──────────────────────────────────────────────────────────────


class GazeTrack:
    """Per-frame gaze of one video (``nan`` without a gaze)."""

    def __init__(self, n: int) -> None:
        self.n = n
        self.yaw = np.full(n, np.nan)
        self.pitch = np.full(n, np.nan)
        self.origin = np.full((n, 3), np.nan)
        self.direction = np.full((n, 3), np.nan)
        self.face = np.zeros(n, bool)

    def trim(self, n: int) -> None:
        self.n = n
        for name in ("yaw", "pitch", "origin", "direction", "face"):
            setattr(self, name, getattr(self, name)[:n])


def measure(video: Path, session: Path, cam: int, min_conf: float, progress) -> tuple:
    """Gaze per frame, frame times (elapsed ms), fps, the camera matrix and
    whether it is calibrated."""
    import cv2
    import run_face_dynamics as rfd
    from eye_contact.gaze import GazeModel, gaze_angles
    from face_dynamics.extract import FaceTracker

    cap = cv2.VideoCapture(str(video))
    if not cap.isOpened():
        raise RuntimeError(f"cannot open {video}")
    fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    k, d, calibrated = rfd.camera_intrinsics(session, cam, w, h)
    times = rfd.frame_times_ms(rfd.read_timestamps_ms(video, cam), total, fps)
    tracker = FaceTracker(k, d, min_confidence=min_conf)
    model = GazeModel(k, d)
    g = GazeTrack(total)
    i = 0
    try:
        while i < total:
            ok, frame = cap.read()
            if not ok:
                break
            face = tracker.measure(frame)
            if face is not None:
                g.face[i] = True
                fg = model.gaze(face.landmarks, face.pose)
                if fg is not None:
                    g.yaw[i], g.pitch[i] = gaze_angles(fg.direction)
                    g.origin[i] = fg.origin
                    g.direction[i] = fg.direction
            i += 1
            progress(i)
    finally:
        tracker.close()
        cap.release()
    if i < total:
        g.trim(i)
    return g, times[: g.n], fps, k, d, calibrated


# ── Conversation context ─────────────────────────────────────────────────────


def load_conversation(session: Path, video: Path):
    """Speaking/listening intervals, the subject's turns and the response
    gaps (all elapsed ms) from Conversation Timing, or ``None``."""
    path = session / "conversation" / f"{video.stem}.conversation.json"
    if not path.exists():
        return None
    doc = json.loads(path.read_text(encoding="utf-8"))
    spurts = doc.get("spurts", [])
    sig = doc.get("signals", {})
    start = sig.get("start_ms")
    analysed = doc.get("summary", {}).get("coverage", {}).get("analysed_s")
    window = (start, start + analysed * 1000.0) if start is not None and analysed else None
    return {
        "window": window,
        "subject": [(s["start_ms"], s["end_ms"]) for s in spurts if s["speaker"] == "subject"],
        "other": [(s["start_ms"], s["end_ms"]) for s in spurts if s["speaker"] == "other"],
        "turns": [
            (t["start_ms"], t["end_ms"]) for t in doc.get("turns", []) if t["speaker"] == "subject"
        ],
        "gaps": [
            (x["prev_end_ms"], x["next_start_ms"])
            for x in doc.get("transitions", [])
            if x["to"] == "subject" and x["fto_ms"] > 0
        ],
    }


# ── Analysis ─────────────────────────────────────────────────────────────────


class NoGaze(RuntimeError):
    """Too little gaze to analyse (an empty result says so instead)."""


def analyse(g: GazeTrack, times_ms: np.ndarray, conv, args) -> dict:
    from eye_contact import metrics as ec
    from eye_contact.gaze import gaze_angles
    from face_dynamics.metrics import smooth

    t0 = float(times_ms[0])
    t = (times_ms - t0) / 1000.0
    yaw = smooth(g.yaw, t)
    pitch = smooth(g.pitch, t)
    seen = np.isfinite(yaw) & np.isfinite(pitch)

    speaking = listening = None
    if conv is not None:
        rel = lambda iv: [((a - t0) / 1000.0, (b - t0) / 1000.0) for a, b in iv]  # noqa: E731
        speaking = ec.mask_from_intervals(t, rel(conv["subject"]))
        listening = ec.mask_from_intervals(t, rel(conv["other"])) & ~speaking
        # Frames outside the stretch Conversation Timing analysed (where the
        # audio and the video overlap) are neither, and not silence either.
        if conv.get("window"):
            covered = ec.mask_from_intervals(t, rel([conv["window"]]))
        else:
            covered = np.ones(g.n, bool)

    target = {"method": args.target}
    if args.target == "camera":
        cam_dirs = -g.origin / np.linalg.norm(g.origin, axis=1, keepdims=True)
        ty, tp = np.full(g.n, np.nan), np.full(g.n, np.nan)
        for i in np.flatnonzero(np.all(np.isfinite(cam_dirs), axis=1)):
            ty[i], tp[i] = gaze_angles(cam_dirs[i])
        target_dirs = cam_dirs
        target.update({"yaw": _r(np.nanmedian(ty)), "pitch": _r(np.nanmedian(tp))})
        basis = seen
    else:
        if args.target == "manual":
            ty, tp = float(args.target_yaw), float(args.target_pitch)
            basis = seen
        else:
            basis = seen
            if listening is not None and (listening & seen).sum() * _step(t) >= MIN_LISTENING_S:
                basis = listening & seen
                target["from"] = "listening"
            else:
                target["from"] = "all frames"
                log(
                    "Partner's direction found from all frames"
                    + (
                        " (less than 20 s of listening)"
                        if conv is not None
                        else " (run Conversation Timing first to use listening time only)"
                    )
                    + "."
                )
            mode = ec.find_mode(yaw[basis], pitch[basis])
            if mode is None:
                raise NoGaze("too few frames with a gaze to find the partner's direction")
            ty, tp = mode
        target.update({"yaw": _r(ty), "pitch": _r(tp)})
        target_dirs = np.broadcast_to(ec.to_direction(ty, tp), (g.n, 3))

    dirs = ec.to_direction(yaw, pitch)
    offsets = ec.angle_between(dirs, target_dirs)
    if args.target == "auto" and args.radius is None:
        radius, sigma, inside = ec.contact_radius(offsets[basis])
    else:
        # The spread around a direction that was not found from the gaze
        # would mix the gaze's own bias into it, so the cone is fixed.
        radius = float(args.radius) if args.radius is not None else FIXED_RADIUS_DEG
        sigma = float("nan")
        known = offsets[basis & np.isfinite(offsets)]
        inside = float(np.mean(known <= radius)) if known.size else 0.0
    target.update(
        {"radius_deg": _r(radius), "spread_deg": _r(sigma), "share_inside_pct": _r(100 * inside)}
    )
    if inside < MIN_TARGET_SHARE and args.target == "auto":
        log(
            f"Only {100 * inside:.0f}% of the gaze falls near the partner's direction found: "
            "there may be no clear partner (check the annotated video, or give --target)."
        )

    state = ec.contact_states(offsets, radius, t)
    tgt_yaw = ty
    tgt_pitch = tp
    av = ec.aversions(state, t, yaw, pitch, tgt_yaw, tgt_pitch)
    summary = ec.summarise(state, t, av, speaking, listening, covered if conv is not None else None)
    summary["face_seen_pct"] = _r(100.0 * float(g.face.mean()))
    if conv is not None:
        rel = lambda iv: [((a - t0) / 1000.0, (b - t0) / 1000.0) for a, b in iv]  # noqa: E731
        summary["turns"] = ec.turn_patterns(state, t, av, rel(conv["turns"]), rel(conv["gaps"]))
    return {
        "t0_ms": t0,
        "t": t,
        "yaw": yaw,
        "pitch": pitch,
        "offsets": offsets,
        "state": state,
        "aversions": av,
        "target": target,
        "target_yaw": np.broadcast_to(np.asarray(ty, float), (g.n,)),
        "target_pitch": np.broadcast_to(np.asarray(tp, float), (g.n,)),
        "speaking": speaking,
        "listening": listening,
        "covered": covered if conv is not None else None,
        "summary": summary,
    }


def empty_analysis(g: GazeTrack, times_ms: np.ndarray, args) -> dict:
    """The result of a video with (almost) no gaze: everything unknown."""
    nan = np.full(g.n, np.nan)
    return {
        "t0_ms": float(times_ms[0]),
        "t": (times_ms - times_ms[0]) / 1000.0,
        "yaw": nan,
        "pitch": nan,
        "offsets": nan,
        "state": nan,
        "aversions": [],
        "target": {"method": args.target, "yaw": None, "pitch": None, "radius_deg": None},
        "target_yaw": nan,
        "target_pitch": nan,
        "speaking": None,
        "listening": None,
        "covered": None,
        "summary": {
            "measured_s": 0.0,
            "measured_pct": 0.0,
            "eye_contact_pct": None,
            "aversions": 0,
            "aversions_per_min": None,
            "aversion_median_s": None,
            "aversion_directions": {"up": 0, "down": 0, "left": 0, "right": 0},
            "face_seen_pct": _r(100.0 * float(g.face.mean())),
        },
    }


def _step(t: np.ndarray) -> float:
    return float(np.median(np.diff(t))) if len(t) > 1 else 0.0


def _r(x, nd: int = 2):
    x = float(x)
    return None if not math.isfinite(x) else round(x, nd)


# ── Outputs ──────────────────────────────────────────────────────────────────


def write_outputs(
    out_dir: Path,
    video: Path,
    cam: int,
    fps: float,
    times_ms,
    g: GazeTrack,
    a: dict,
    settings,
    video_rel,
):
    out_dir.mkdir(parents=True, exist_ok=True)
    t0 = a["t0_ms"]
    frames = []
    for i in range(g.n):
        f = {
            "frame_index": i,
            "timestamp_ms": int(round(times_ms[i])),
            "face_detected": bool(g.face[i]),
        }
        st = a["state"][i]
        if math.isfinite(a["yaw"][i]):
            f.update(
                {
                    "gaze_yaw": _r(a["yaw"][i]),
                    "gaze_pitch": _r(a["pitch"][i]),
                    "offset_deg": _r(a["offsets"][i]),
                }
            )
        f["contact"] = None if not math.isfinite(st) else bool(st == 1.0)
        frames.append(f)
    doc = {
        "schema": SCHEMA,
        "source_video": video.name,
        "camera_index": cam,
        "fps": round(fps, 3),
        "settings": settings,
        "annotated_video": video_rel,
        "target": a["target"],
        "summary": a["summary"],
        "aversions": [
            {
                "start_ms": int(round(t0 + x["start_s"] * 1000.0)),
                "end_ms": int(round(t0 + x["end_s"] * 1000.0)),
                "duration_ms": round(x["duration_s"] * 1000.0),
                "offset_deg": _r(x["offset_deg"]),
                "direction": x["direction"],
            }
            for x in a["aversions"]
        ],
        "frames": frames,
    }
    path = out_dir / f"{video.stem}.eye_contact.json"
    path.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    with (out_dir / f"{video.stem}.eye_contact.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(
            [
                "frame_index",
                "timestamp_ms",
                "time_s",
                "face_detected",
                "gaze_yaw",
                "gaze_pitch",
                "offset_deg",
                "contact",
                "state",
            ]
        )
        for i in range(g.n):
            st = a["state"][i]
            speaking = a["speaking"] is not None and bool(a["speaking"][i])
            listening = a["listening"] is not None and bool(a["listening"][i])
            covered = a["covered"] is not None and bool(a["covered"][i])
            talk = "speaking" if speaking else ("listening" if listening else "")
            if not talk and covered:
                talk = "silence"
            w.writerow(
                [
                    i,
                    int(round(times_ms[i])),
                    f"{a['t'][i]:.3f}",
                    int(g.face[i]),
                    "" if not math.isfinite(a["yaw"][i]) else f"{a['yaw'][i]:.2f}",
                    "" if not math.isfinite(a["pitch"][i]) else f"{a['pitch'][i]:.2f}",
                    "" if not math.isfinite(a["offsets"][i]) else f"{a['offsets'][i]:.2f}",
                    "" if not math.isfinite(st) else int(st),
                    talk,
                ]
            )
    with (out_dir / f"{video.stem}.aversions.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["start_s", "end_s", "duration_ms", "offset_deg", "direction"])
        for x in a["aversions"]:
            w.writerow(
                [
                    f"{x['start_s']:.3f}",
                    f"{x['end_s']:.3f}",
                    round(x["duration_s"] * 1000),
                    f"{x['offset_deg']:.1f}",
                    x["direction"],
                ]
            )
    return path


# ── Annotated video ──────────────────────────────────────────────────────────

GREEN, AMBER, GREY, WHITE = (90, 220, 90), (40, 190, 240), (150, 150, 150), (235, 235, 235)
MAP_RANGE_DEG = 30.0  # the gaze map shows +-this around the partner
TRAIL_S = 1.0


def render(
    video: Path, out_path: Path, g: GazeTrack, a: dict, k: np.ndarray, d: np.ndarray, progress
) -> bool:
    import cv2
    from eye_contact.metrics import direction_label
    from gaze.render import open_writer, partial_path

    cap = cv2.VideoCapture(str(video))
    fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    partial = partial_path(out_path)
    writer = open_writer(partial, fps, (w, h))
    if writer is None:
        cap.release()
        log("Cannot open a video writer; no annotated video.")
        return False
    scale = max(0.5, h / 720.0)
    font = cv2.FONT_HERSHEY_SIMPLEX
    radius = a["target"]["radius_deg"] or 8.0
    trail_n = max(1, int(round(TRAIL_S * fps)))
    dy_all = a["yaw"] - a["target_yaw"]
    dp_all = a["pitch"] - a["target_pitch"]

    def text(img, s, org, color, size=0.7, thick=2):
        fg = max(1, int(round(thick * scale)))
        (tw, th), base = cv2.getTextSize(s, font, size * scale, fg)
        x, y = org
        pad = max(2, int(3 * scale))
        box = img[max(0, y - th - pad) : y + base + pad, max(0, x - pad) : x + tw + pad]
        box[:] = (box * 0.35).astype(img.dtype)
        cv2.putText(img, s, org, font, size * scale, color, fg, cv2.LINE_AA)

    # Gaze-map positions of every frame, computed once.
    size = int(170 * scale)
    x0, y0 = w - size - int(12 * scale), int(12 * scale)
    c = (x0 + size // 2, y0 + size // 2)
    px_per_deg = size / 2 / MAP_RANGE_DEG
    map_ok = np.isfinite(dy_all) & np.isfinite(dp_all)
    map_u = np.clip(c[0] + np.nan_to_num(dy_all) * px_per_deg, x0, x0 + size - 1).astype(np.int32)
    map_v = np.clip(c[1] - np.nan_to_num(dp_all) * px_per_deg, y0, y0 + size - 1).astype(np.int32)

    done = False
    try:
        for i in range(g.n):
            ok, img = cap.read()
            if not ok:
                break
            st = a["state"][i]
            if np.all(np.isfinite(g.origin[i])) and np.all(np.isfinite(g.direction[i])):
                pts = np.array([g.origin[i], g.origin[i] + 300.0 * g.direction[i]])
                px, _ = cv2.projectPoints(pts.reshape(-1, 1, 3), np.zeros(3), np.zeros(3), k, d)
                p0, p1 = px.reshape(-1, 2)
                if np.all(np.isfinite(px)) and np.all(np.abs(px) < 4 * max(w, h)):
                    color = GREEN if st == 1.0 else AMBER
                    cv2.arrowedLine(
                        img,
                        tuple(int(v) for v in p0),
                        tuple(int(v) for v in p1),
                        color,
                        max(2, int(3 * scale)),
                        cv2.LINE_AA,
                        tipLength=0.15,
                    )
            if st == 1.0:
                label, color = "EYE CONTACT", GREEN
            elif st == 0.0:
                off = (dy_all[i], dp_all[i])
                label = "LOOKING AWAY"
                if np.all(np.isfinite(off)):
                    label += f" ({direction_label(*off)})"
                color = AMBER
            else:
                label, color = "no gaze", GREY
            text(img, label, (int(14 * scale), int(36 * scale)), color, 0.9, 2)
            if a["speaking"] is not None:
                sp = "speaking" if a["speaking"][i] else ("listening" if a["listening"][i] else "")
                if sp:
                    text(img, sp, (int(14 * scale), int(70 * scale)), WHITE, 0.6, 1)
            # Gaze map: the partner at the centre, the contact cone, the gaze
            # over the last second (the subject's left is drawn to the right,
            # as the camera sees it).
            roi = img[y0 : y0 + size, x0 : x0 + size]
            roi[:] = (roi * 0.35).astype(img.dtype)
            cv2.circle(img, c, int(radius * px_per_deg), GREEN, 1, cv2.LINE_AA)
            cv2.drawMarker(img, c, WHITE, cv2.MARKER_CROSS, int(10 * scale), 1)
            for j in range(max(0, i - trail_n), i + 1):
                if map_ok[j]:
                    r = max(2, int(5 * scale)) if j == i else max(1, int(2 * scale))
                    color = GREEN if a["state"][j] == 1.0 else AMBER
                    cv2.circle(img, (int(map_u[j]), int(map_v[j])), r, color, -1, cv2.LINE_AA)
            cv2.putText(
                img, "partner", (x0 + 4, y0 + size - 6), font, 0.4 * scale, WHITE, 1, cv2.LINE_AA
            )
            writer.write(img)
            progress(i + 1)
        else:
            done = True
    finally:
        writer.release()
        cap.release()
        if not done:
            partial.unlink(missing_ok=True)
    if not done:
        log("The video ended early while drawing; no annotated video.")
        return False
    partial.replace(out_path)
    return True


# ── Main ─────────────────────────────────────────────────────────────────────


def process_camera(session: Path, video: Path, cam: int, args) -> bool:
    import cv2

    t_start = time.perf_counter()
    cap = cv2.VideoCapture(str(video))
    n = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    cap.release()
    passes = 1 if args.no_video else 2
    total = max(1, passes * n)
    every = max(1, n // 100)
    g, times_ms, fps, k, d, calibrated = measure(
        video,
        session,
        cam,
        args.min_confidence,
        lambda i: progress_line(i, total, t_start) if i % every == 0 else None,
    )
    if not calibrated:
        log(
            "No intrinsic calibration for this camera: a nominal lens is used "
            "(fine for eye contact)."
        )
    if np.isfinite(g.yaw).mean() < 0.2:
        log(
            f"Gaze measured in only {100 * np.isfinite(g.yaw).mean():.0f}% of frames: "
            "the face must be visible with its eyes."
        )
    conv = load_conversation(session, video)
    if conv is None:
        log("No Conversation Timing output: eye contact is not split by speaking and listening.")
    try:
        a = analyse(g, times_ms, conv, args)
    except NoGaze as exc:
        log(f"{video.name}: {exc}; writing an empty result.")
        a = empty_analysis(g, times_ms, args)

    out_dir = session / "eye_contact"
    out_video = out_dir / f"{video.stem}.eye_contact.mp4"
    video_rel = None
    if not args.no_video:
        out_dir.mkdir(parents=True, exist_ok=True)
        if render(
            video,
            out_video,
            g,
            a,
            k,
            d,
            lambda i: progress_line(n + i, total, t_start) if i % every == 0 else None,
        ):
            video_rel = f"{out_dir.name}/{out_video.name}"
    if video_rel is None and out_video.exists():
        out_video.unlink()

    settings = {
        "min_confidence": args.min_confidence,
        "target": args.target,
        "intrinsics": "calibrated" if calibrated else "nominal",
        "conversation": conv is not None,
    }
    path = write_outputs(out_dir, video, cam, fps, times_ms, g, a, settings, video_rel)
    s, tg = a["summary"], a["target"]
    log(
        f"Done in {time.perf_counter() - t_start:.1f}s: "
        f"partner at yaw {tg['yaw']}, pitch {tg['pitch']} "
        f"(cone {tg['radius_deg']} deg); eye contact {s['eye_contact_pct']}%, "
        f"{s['aversions']} look-aways"
        + (
            f"; listening {s.get('eye_contact_listening_pct')}%, "
            f"speaking {s.get('eye_contact_speaking_pct')}%"
            if conv is not None
            else ""
        )
    )
    print(f"  100.0%  ({total}/{total})", flush=True)
    log(f"Results -> {path}")
    return True


def main(argv=None) -> int:
    import run_face_dynamics as rfd

    args = parse_args(argv)
    session = Path(args.session)
    videos = sorted((session / "video").glob("video_*.mp4"))
    if args.camera is not None:
        videos = [v for v in videos if rfd.camera_index_from_filename(v) == args.camera]
    if not videos:
        print(f"{TAG} No videos to analyse in {session / 'video'}", file=sys.stderr, flush=True)
        return 1
    failures = 0
    for pos, video in enumerate(videos, start=1):
        print(f"{TAG} Camera {pos}/{len(videos)}: {video.name}", flush=True)
        try:
            ok = process_camera(session, video, rfd.camera_index_from_filename(video), args)
        except Exception:  # one camera's failure must not stop the others
            traceback.print_exc()
            log(f"{video.name}: failed (see the error above)")
            ok = False
        failures += not ok
    return 1 if failures == len(videos) else 0


if __name__ == "__main__":
    sys.exit(main())
