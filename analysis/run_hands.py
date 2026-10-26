"""
MOSAIC Hands: 21 points per hand (wrist, each finger joint and fingertip) on
each camera, with whose hand it is and which side, from the camera's Pose
result. Made for close-up cameras on a table, where hands are large enough
to see the fingers.

    python analysis/run_hands.py --session DIR [--model rtmw|mediapipe]
        [--camera N ...] [--stride 1] [--no-video]

Writes, per camera, in ``<session>/hands/``:

* ``video_N.hands.json``: the model, and a summary per hand (person, side,
  how long it was seen and when, its size in pixels, its mean score, and
  its kinematics: wrist path and speeds, grip aperture, hand opening);
* ``video_N.hands.frames.json``: every analysed frame's hands: person,
  side, score, the 21 points (pixels) and their scores;
* ``video_N.hands.kinematics.csv``: per hand and analysed frame, the
  wrist's position, speeds (pixels and hand lengths per second), grip
  aperture, hand opening and each finger's bend;
* ``video_N.hands.movements.csv``: one row per movement of a hand (a
  reach, a grasp, any move): when, how long, peak speed and when it came,
  path and straightness, movement units, SPARC smoothness, the maximum grip
  aperture and when it came;
* ``video_N.hands.mp4``: the video with the hands drawn on it, the
  person's left hand in green and right in orange.

When two or more cameras with a room calibration have a Hands result, the
hands they both see are also put in the room, in millimetres, in
``<session>/hands/``:

* ``hands3d.json``: per hand (one side of one person, kept over time) its
  cameras, when it was seen, its kinematics and movements in millimetres;
  and per pair of cameras how well they agreed (how far their lines of
  sight through the same points miss, the steady part of that, which is
  their room calibration together, and the scatter, in millimetres);
* ``hands3d.frames.json``: every tick's hands, 21 points in millimetres;
* ``hands3d.kinematics.csv`` and ``hands3d.movements.csv``: as per camera,
  in millimetres as well.

``--kinematics-only`` works the kinematics and the 3D hands out again from
the points already found, without the models.
See docs/math/hands.rst.
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
from camera_pick import add_camera_arg, pick_cameras
from hands import hands3d, kinematics, movements
from hands.people import EDGES, KEYPOINT_NAMES, seen_spans, summarise
from run_face_dynamics import frame_times_ms, read_timestamps_ms
from run_objects import load_pose, nearest_people, video_info
from video_publish import open_writer, publish_video

TAG = "[run_hands]"
SCHEMA = "mosaic-hands-v1"
MODELS = ("rtmw", "mediapipe")
#: BGR colours of the person's left and right hand, and of a hand no wrist
#: was near.
SIDE_COLOURS = {"left": (90, 200, 90), "right": (60, 160, 230), "unknown": (170, 170, 170)}


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="MOSAIC hands and fingers")
    p.add_argument("--session", required=True, metavar="DIR")
    add_camera_arg(p)
    p.add_argument("--model", choices=MODELS, default="rtmw", help="Hand model (default: rtmw)")
    p.add_argument("--stride", type=int, default=1, help="Analyse every Nth frame (default: 1)")
    p.add_argument("--no-video", action="store_true", help="Do not write the annotated video")
    p.add_argument(
        "--kinematics-only",
        action="store_true",
        help="Work the kinematics out again from the points already found (no models)",
    )
    return p.parse_args(argv)


def log(msg: str) -> None:
    print(f"{TAG} {msg}", flush=True)


def rounded(hand: dict) -> dict:
    """A hand as saved: points to 0.1 px, scores to 0.01. A person Pose did
    not track has a negative id, a new one each frame: saved as None."""
    pid = hand.get("subject_id")
    return {
        "subject_id": pid if pid is None or pid >= 0 else None,
        "side": hand["side"],
        "score": round(float(hand["score"]), 3),
        "keypoints": [[round(float(x), 1), round(float(y), 1)] for x, y in hand["keypoints"]],
        "scores": [round(float(s), 2) for s in hand["scores"]],
    }


def analyse_camera(session: Path, video: Path, cam: int, model: str, stride: int, estimator):
    """The hands in one camera's video, or None without a Pose result
    (it says where the people are)."""
    import cv2

    pose = load_pose(session, cam)
    if not pose:
        return None
    keys = sorted(pose)
    reach = max(1, int(np.median(np.diff(keys))) // 2) if len(keys) > 1 else 1
    n, fps, _size = video_info(video)
    times = frame_times_ms(read_timestamps_ms(video, cam), max(n, 1), fps)
    frames = []
    cap = cv2.VideoCapture(str(video))
    t_start = time.perf_counter()
    idx = 0
    try:
        while True:
            ok, img = cap.read()
            if not ok:
                break
            if idx % stride == 0:
                people = nearest_people(pose, keys, idx, reach)
                rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
                hands = estimator(rgb, people) if people else []
                frames.append(
                    {
                        "frame_index": idx,
                        "timestamp_ms": round(float(times[min(idx, len(times) - 1)]), 1),
                        "hands": [rounded(h) for h in hands],
                    }
                )
                if n and len(frames) % 25 == 0:
                    print(
                        f"  {min(100.0, 100.0 * (idx + 1) / n):5.1f}%  ({idx + 1}/{n})  "
                        f"{time.perf_counter() - t_start:.1f}s elapsed",
                        flush=True,
                    )
            idx += 1
    finally:
        cap.release()
    step_ms = 1000.0 * stride / fps
    return {
        "schema": SCHEMA,
        "source_video": f"video/{video.name}",
        "camera_index": cam,
        "model": model,
        "stride": stride,
        "fps": fps,
        # The video's first frame, session elapsed ms: the chart's zero.
        "video_start_ms": round(float(times[0]), 1),
        "keypoint_names": KEYPOINT_NAMES,
        "hands": summarise(frames, step_ms),
        "frames": frames,
    }


def _write_csv(path: Path, columns, rows: list) -> None:
    partial = path.with_name(path.name + ".partial")
    with partial.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=columns)
        w.writeheader()
        w.writerows(rows)
    partial.replace(path)


def add_kinematics(res: dict, frames: list, csv_path: Path) -> None:
    """Each hand's kinematics and movements: every analysed frame's into
    ``csv_path``, every movement's into the ``.movements.csv`` beside it,
    the summaries into the hand's entry in ``res["hands"]``, and how each
    person used their two hands into ``res["people"]``."""
    rate = float(res["fps"]) / max(1, int(res.get("stride", 1)))
    if rate < kinematics.MIN_RATE_HZ:
        log(
            f"No kinematics: {rate:.1f} analysed frames per second, under "
            f"{kinematics.MIN_RATE_HZ:g}; analyse every frame (or every 2nd) for them"
        )
    by_hand = kinematics.hand_samples(frames)
    rows, moves, by_person = [], [], {}
    for h in res["hands"]:
        h.pop("kinematics", None)
        h.pop("movements", None)
        series = kinematics.hand_series(by_hand.get((h["subject_id"], h["side"]), []), rate)
        if series is None:
            continue
        h["kinematics"] = kinematics.summary(series, rate)
        found, cut_off = movements.find_movements(series, rate)
        h["movements"] = movements.hand_summary(found, len(cut_off))
        moves += [{"subject_id": h["subject_id"], "side": h["side"], **m} for m in found]
        by_person.setdefault(h["subject_id"], {})[h["side"]] = found + cut_off
        for i in range(len(series["time_ms"])):
            row = {"subject_id": h["subject_id"], "side": h["side"]}
            for col in kinematics.COLUMNS:
                if col in series:
                    v = float(series[col][i])
                    row[col] = "" if math.isnan(v) else round(v, 1 if col == "time_ms" else 4)
            rows.append(row)
    _write_csv(csv_path, kinematics.COLUMNS, rows)
    res["kinematics_file"] = f"hands/{csv_path.name}"
    moves_path = csv_path.with_name(csv_path.name.replace(".kinematics.", ".movements."))
    moves.sort(key=lambda m: (m["start_ms"], m["subject_id"], m["side"]))
    _write_csv(moves_path, movements.MOVEMENT_COLUMNS, moves)
    res["movements_file"] = f"hands/{moves_path.name}"
    # Two hands' use, for the people with both hands measured.
    res["people"] = [
        {"subject_id": pid, **movements.person_summary(sides["left"], sides["right"])}
        for pid, sides in sorted(by_person.items(), key=lambda kv: kv[0])
        if "left" in sides and "right" in sides
    ]


def kinematics_only(session: Path, video: Path) -> Path | None:
    """The kinematics worked out again from a camera's saved points; None
    when that camera has no Hands result."""
    path = session / "hands" / f"{video.stem}.hands.json"
    frames_path = session / "hands" / f"{video.stem}.hands.frames.json"
    if not path.exists() or not frames_path.exists():
        return None
    res = json.loads(path.read_text(encoding="utf-8"))
    frames = json.loads(frames_path.read_text(encoding="utf-8"))["frames"]
    add_kinematics(res, frames, session / "hands" / f"{video.stem}.hands.kinematics.csv")
    path.write_text(json.dumps(res, indent=1), encoding="utf-8")
    return path


def try_kinematics(res: dict, frames: list, csv_path: Path) -> None:
    """:func:`add_kinematics`, a failure logged and left out: the points
    found are kept either way."""
    try:
        add_kinematics(res, frames, csv_path)
    except Exception:
        traceback.print_exc()
        for key in ("kinematics_file", "movements_file", "people"):
            res.pop(key, None)
        for h in res["hands"]:
            h.pop("kinematics", None)
            h.pop("movements", None)
        log(f"{csv_path.name}: kinematics failed (see the error above); the points are kept")


def add_hands3d(session: Path) -> Path | None:
    """The hands two or more calibrated cameras see, in the room (see the
    module docstring); None, logged, when there are not two such cameras.
    An earlier 3D result is removed first: it would no longer be current."""
    for name in ("hands3d.json", "hands3d.frames.json", "hands3d.kinematics.csv",
                 "hands3d.movements.csv"):  # fmt: skip
        (session / "hands" / name).unlink(missing_ok=True)
    meta_path, manifest_path = session / "session_meta.json", session / "sync_manifest.json"
    if not meta_path.exists() or not manifest_path.exists():
        return None
    meta = json.loads(meta_path.read_text(encoding="utf-8-sig"))
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    geoms = hands3d.camera_geoms(meta)
    cams, strides = {}, []
    for c in sorted(geoms):
        res_path = session / "hands" / f"video_{c}.hands.json"
        frames_path = session / "hands" / f"video_{c}.hands.frames.json"
        if res_path.exists() and frames_path.exists():
            strides.append(json.loads(res_path.read_text(encoding="utf-8")).get("stride", 1))
            frames = json.loads(frames_path.read_text(encoding="utf-8"))["frames"]
            cams[c] = hands3d.CameraHands(frames, session / "video" / f"timestamps_cam{c}.csv")
    if len(cams) < 2:
        if len(geoms) >= 2:
            log("3D hands: Hands has been run on fewer than two calibrated cameras")
        return None
    master = float(manifest.get("master_fps", 25.0))
    step_ns = int(manifest.get("step_ns", int(1e9 / master)))
    origin_ns = int(manifest.get("t_origin_ns", 0))
    # The 3D samples come at the ticks, every Nth frame of the cameras: the
    # kinematics' rate (and its minimum) follows the slowest.
    rate = master / max(1, max(strides))
    ids = manifest.get("ticks", {}) or {}
    ticks, compared, matched, last_id = [], 0, 0, {}
    for tick in range(int(manifest.get("total_ticks", 0))):
        per_camera, frame_of = {}, {}
        for c, ch in cams.items():
            frame_ids = ids.get(f"cam{c}_frame_ids", [])
            if tick >= len(frame_ids) or frame_ids[tick] < 0:
                continue
            hands_seen = ch.at_frame_id(frame_ids[tick])
            if hands_seen is not None:
                per_camera[c] = hands_seen
                frame_of[c] = frame_ids[tick]
        # A camera free-running slower than the ticks names one frame for
        # two ticks: a tick that repeats a camera's last frame would put an
        # old view with a new one, so it is skipped.
        if len(per_camera) < 2 or any(last_id.get(c) == f for c, f in frame_of.items()):
            continue
        last_id.update(frame_of)
        found, n_compared, n_matched = hands3d.match_tick(per_camera, geoms)
        compared, matched = compared + n_compared, matched + n_matched
        deltas = {c: (ids.get(f"cam{c}_delta_ms") or [None] * (tick + 1))[tick] for c in per_camera}
        for h in found:
            h["frame_gap_ms"] = {
                (a, b): abs(deltas[a] - deltas[b])
                for a, b in h["gaps"]
                if deltas.get(a) is not None and deltas.get(b) is not None
            }
        if found:
            ticks.append(((origin_ns + tick * step_ns) / 1e6, found))
    hands3d.track(ticks)
    # Whose hand: the Pose id on one reference camera (the lowest-numbered
    # with a result), so that ids of different cameras are never mixed; a
    # hand that camera never saw is someone's.
    ref = min(cams)
    by_hand: dict = {}
    for t, found in ticks:
        for h in found:
            e = by_hand.setdefault(h["hand"], {"side": h["side"], "samples": [], "ids": {}})
            e["samples"].append(
                (t, h["points"], np.where(np.isfinite(h["points"][:, 0]), 1.0, 0.0))
            )
            for c, v in h["views"].items():
                e["ids"].setdefault(c, []).append(v["subject_id"])
    summary_hands, rows, moves, by_person = [], [], [], {}
    for n, e in sorted(by_hand.items()):
        on_ref = [i for i in e["ids"].get(ref, []) if i is not None]
        subject = max(set(on_ref), key=on_ref.count) if on_ref else None
        entry = {
            "hand": n,
            "subject_id": subject,
            "subject_camera": ref,
            "side": e["side"],
            "cameras": sorted(e["ids"]),
            "ticks": len(e["samples"]),
            "seen_spans": [
                [round(a, 1), round(b + 1000.0 / rate, 1)]
                for a, b in seen_spans([s[0] for s in e["samples"]], 500.0)
            ],
        }
        series = kinematics.hand_series(e["samples"], rate, unit="mm")
        if series is not None:
            entry["kinematics"] = kinematics.summary(series, rate)
            found, cut_off = movements.find_movements(series, rate)
            entry["movements"] = movements.hand_summary(found, len(cut_off))
            tag = {"hand": n, "subject_id": subject, "side": e["side"]}
            moves += [{**tag, **m} for m in found]
            by_person.setdefault(subject, {}).setdefault(e["side"], []).extend(found + cut_off)
            for i in range(len(series["time_ms"])):
                row = dict(tag)
                for col in kinematics.COLUMNS_MM:
                    if col in series:
                        v = float(series[col][i])
                        row[col] = "" if math.isnan(v) else round(v, 1 if col == "time_ms" else 4)
                rows.append(row)
        summary_hands.append(entry)
    out = session / "hands"
    _write_csv(out / "hands3d.kinematics.csv", kinematics.COLUMNS_MM, rows)
    moves.sort(key=lambda m: (m["start_ms"], m["hand"]))
    _write_csv(out / "hands3d.movements.csv", movements.MOVEMENT_COLUMNS_MM, moves)
    frames = [
        {
            "timestamp_ms": round(t, 1),
            "hands": [
                {
                    "hand": h["hand"],
                    "side": h["side"],
                    "cameras": sorted(h["views"]),
                    "points_mm": [
                        None if not np.isfinite(p).all() else [round(float(x), 1) for x in p]
                        for p in h["points"]
                    ],
                }
                for h in found
            ],
        }
        for t, found in ticks
    ]
    (out / "hands3d.frames.json").write_text(
        json.dumps({"schema": "mosaic-hands3d-v1-frames", "frames": frames}), encoding="utf-8"
    )
    result = {
        "schema": "mosaic-hands3d-v1",
        "master_fps": master,
        "keypoint_names": KEYPOINT_NAMES,
        "rate": rate,
        "agreement": hands3d.pair_report(ticks, compared, matched),
        "hands": summary_hands,
        "people": [
            {"subject_id": pid, **movements.person_summary(s["left"], s["right"])}
            for pid, s in sorted(by_person.items(), key=lambda kv: (kv[0] is None, kv[0] or 0))
            if pid is not None and "left" in s and "right" in s
        ],
        "frames_file": "hands/hands3d.frames.json",
        "kinematics_file": "hands/hands3d.kinematics.csv",
        "movements_file": "hands/hands3d.movements.csv",
    }
    path = out / "hands3d.json"
    path.write_text(json.dumps(result, indent=1), encoding="utf-8")
    report = ", ".join(
        f"Cameras {p['cameras'][0] + 1} and {p['cameras'][1] + 1}: sight lines miss by "
        f"{p['gap_median_mm']:.0f} mm (steady {p['offset_mm']:.0f}, scatter "
        f"{p['scatter_median_mm']:.0f})"
        for p in result["agreement"]["camera_pairs"]
    )
    log(f"3D hands: {len(summary_hands)} hand(s), {len(ticks)} ticks; agreement {report} -> {path}")
    return path


def try_hands3d(session: Path) -> None:
    """:func:`add_hands3d`, a failure logged: the cameras' results stay."""
    try:
        add_hands3d(session)
    except Exception:
        traceback.print_exc()
        log("3D hands failed (see the error above); each camera's result is kept")


def write_video(video: Path, result: dict, out_path: Path) -> bool:
    """The video with each hand's points and lines, and whose it is; between
    analysed frames the last result holds."""
    import cv2
    from gaze.render import partial_path

    by_index = {f["frame_index"]: f["hands"] for f in result["frames"]}
    cap = cv2.VideoCapture(str(video))
    fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
    w, h = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)), int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    partial = partial_path(out_path)
    writer = open_writer(partial, fps, (w, h))
    if writer is None:
        cap.release()
        return False
    shown, idx = [], 0
    try:
        while True:
            ok, img = cap.read()
            if not ok:
                break
            shown = by_index.get(idx, shown)
            for hand in shown:
                colour = SIDE_COLOURS.get(hand["side"], SIDE_COLOURS["unknown"])
                pts = np.asarray(hand["keypoints"], float).astype(int)
                for a, b in EDGES:
                    cv2.line(img, tuple(pts[a]), tuple(pts[b]), colour, 2, cv2.LINE_AA)
                for p in pts:
                    cv2.circle(img, tuple(p), 3, colour, -1, cv2.LINE_AA)
                who = "?" if hand["subject_id"] is None else f"S{hand['subject_id']}"
                x, y = pts[:, 0].min(), max(14, pts[:, 1].min() - 8)
                text = f"{who} {hand['side']}"
                cv2.putText(img, text, (int(x), int(y)), cv2.FONT_HERSHEY_SIMPLEX, 0.6, colour, 2)
            writer.write(img)
            idx += 1
    finally:
        cap.release()
        writer.release()
    publish_video(partial, out_path)
    return True


def main(argv=None, estimator=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    args = parse_args(argv)
    session = Path(args.session)
    videos = pick_cameras(sorted((session / "video").glob("video_*.mp4")), args.camera)
    if not videos:
        print(f"{TAG} No videos to analyse in {session / 'video'}", file=sys.stderr)
        return 1
    if args.kinematics_only:
        failed = 0
        for video in videos:
            try:
                path = kinematics_only(session, video)
            except Exception:  # one camera's failure must not stop the others
                traceback.print_exc()
                log(f"{video.name}: kinematics failed (see the error above)")
                failed += 1
                continue
            if path is None:
                log(f"{video.name}: no Hands result to work from: run Hands first")
                failed += 1
            else:
                log(f"{video.name}: kinematics -> {path}")
        try_hands3d(session)
        print(f"  100.0%  ({len(videos)}/{len(videos)})", flush=True)
        return 1 if failed == len(videos) else 0
    if estimator is None:
        from hands.models import make_estimator

        log(f"Loading the {args.model} hand model")
        estimator = make_estimator(args.model)
    out_dir = session / "hands"
    out_dir.mkdir(exist_ok=True)
    failed = 0
    for pos, video in enumerate(videos, start=1):
        cam = int(video.stem.split("_")[1])
        log(f"Camera {pos}/{len(videos)}: {video.name}")
        try:
            res = analyse_camera(session, video, cam, args.model, max(1, args.stride), estimator)
            if res is None:
                log(f"{video.name}: no Pose result for this camera: run Pose on it first")
                failed += 1
                continue
            if not args.no_video:
                out_video = out_dir / f"{video.stem}.hands.mp4"
                if write_video(video, res, out_video):
                    res["annotated_video"] = f"hands/{out_video.name}"
            frames = res.pop("frames")
            (out_dir / f"{video.stem}.hands.frames.json").write_text(
                json.dumps({"schema": SCHEMA + "-frames", "frames": frames}), encoding="utf-8"
            )
            res["frames_file"] = f"hands/{video.stem}.hands.frames.json"
            try_kinematics(res, frames, out_dir / f"{video.stem}.hands.kinematics.csv")
            path = out_dir / f"{video.stem}.hands.json"
            path.write_text(json.dumps(res, indent=1), encoding="utf-8")
        except Exception:  # one camera's failure must not stop the others
            traceback.print_exc()
            log(f"{video.name}: failed (see the error above)")
            failed += 1
            continue
        seen = ", ".join(
            f"{'?' if h['subject_id'] is None else 'S' + str(h['subject_id'])} {h['side']} "
            f"{h['seen_ms'] / 1000.0:.1f} s"
            for h in res["hands"]
        )
        log(f"{video.name}: {seen or 'no hand found'} -> {path}")
    try_hands3d(session)
    print(f"  100.0%  ({len(videos)}/{len(videos)})", flush=True)
    return 1 if failed == len(videos) else 0


if __name__ == "__main__":
    sys.exit(main())
