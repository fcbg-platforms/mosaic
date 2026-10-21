"""
MOSAIC Object Tracking: objects named in words (SAM 3), followed through each
camera's video, and when each person's hand touches them.

    python analysis/run_objects.py --session DIR [--objects "toy, cup, tablet"]
        [--camera N] [--stride 2] [--weights sam3.pt] [--no-video]

Writes, per camera, in ``<session>/objects/``:

* ``video_N.objects.json``: a summary per object (how much of the video it
  was seen in and when, how far it moved, how long each person touched it)
  and the contacts: runs where a person's wrist (from the Pose plugin, when
  it was run on that camera) is on or next to an object;
* ``video_N.objects.frames.json``: every analysed frame's objects: id,
  label, score, box and outline (kept apart: it can be large);
* ``video_N.objects.mp4``: the video with the outlines drawn on it.
See docs/math/object_tracking.rst.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import traceback
from pathlib import Path

import numpy as np
from objects.contact import contact_events, frame_contacts, summarise
from run_face_dynamics import frame_times_ms, read_timestamps_ms
from video_publish import open_writer, publish_video

TAG = "[run_objects]"
SCHEMA = "mosaic-objects-v1"
DEFAULT_OBJECTS = "toy, cup, tablet computer"
#: Objects seen for less than this in all are left out of the results (kept
#: in the frames file): brief false detections, a reflection, a passing hand.
MIN_SEEN_S = 1.0
#: Distinct, readable outline colours (BGR), one per object id.
COLOURS = [(90, 200, 90), (230, 160, 60), (80, 140, 240), (200, 90, 200), (60, 200, 220)]


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="MOSAIC object tracking (SAM 3)")
    p.add_argument("--session", required=True, metavar="DIR")
    p.add_argument("--camera", type=int, default=None, help="Only this camera index")
    p.add_argument(
        "--objects",
        default=DEFAULT_OBJECTS,
        help=f"Comma-separated names (default: {DEFAULT_OBJECTS})",
    )
    p.add_argument("--stride", type=int, default=2, help="Analyse every Nth frame (default: 2)")
    p.add_argument("--weights", default="sam3.pt", help="SAM 3 weights (default: sam3.pt)")
    p.add_argument("--no-video", action="store_true", help="Do not write the annotated video")
    return p.parse_args(argv)


def log(msg: str) -> None:
    print(f"{TAG} {msg}", flush=True)


def parse_labels(text: str) -> list:
    """``"toy, cup,,tablet "`` -> ``["toy", "cup", "tablet"]``, no repeats."""
    out = []
    for part in text.split(","):
        name = part.strip()
        if name and name.lower() not in (o.lower() for o in out):
            out.append(name)
    return out


def nearest_people(pose: dict, keys: list, idx: int, reach: int) -> list:
    """The people of the Pose frame nearest decoded frame ``idx``, when one
    is within ``reach`` frames (Pose may have analysed every Nth frame)."""
    if not keys:
        return []
    import bisect

    k = bisect.bisect_left(keys, idx)
    best = min((keys[j] for j in (k - 1, k) if 0 <= j < len(keys)), key=lambda f: abs(f - idx))
    return pose[best] if abs(best - idx) <= reach else []


def load_pose(session: Path, cam: int) -> dict:
    """The camera's latest Pose result: decoded frame index -> people."""
    files = sorted(
        (session / "pose").glob(f"video_{cam}.*.pose.json"), key=lambda p: p.stat().st_mtime
    )
    if not files:
        return {}
    try:
        d = json.loads(files[-1].read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {int(f.get("frame_index", -1)): f.get("subjects") or [] for f in d.get("frames", [])}


def video_info(video: Path) -> tuple:
    import cv2

    cap = cv2.VideoCapture(str(video))
    try:
        n = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
        w, h = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)), int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    finally:
        cap.release()
    return n, fps, (w, h)


def analyse_camera(
    session: Path, video: Path, cam: int, labels: list, weights: Path, stride: int, track
) -> dict:
    """Track the objects in one camera's video; ``track`` is
    :func:`objects.track.track_video` (a stand-in in the tests)."""
    n, fps, _size = video_info(video)
    times = frame_times_ms(read_timestamps_ms(video, cam), max(n, 1), fps)
    pose = load_pose(session, cam)
    keys = sorted(pose)
    # Half of Pose's own step: frames between two Pose frames take the nearer.
    reach = max(1, int(np.median(np.diff(keys))) // 2) if len(keys) > 1 else 1
    step_ms = 1000.0 * stride / fps
    frames, pairs, pair_times = [], [], []
    t_start = time.perf_counter()
    for k, (idx, objects) in enumerate(track(video, labels, weights, stride)):
        t = float(times[min(idx, len(times) - 1)])
        frames.append({"frame_index": idx, "timestamp_ms": round(t, 1), "objects": objects})
        if pose:
            people = nearest_people(pose, keys, idx, reach)
            pairs.append(frame_contacts(objects, people))
            pair_times.append(t)
        if n and k % 10 == 0:
            pct = min(100.0, 100.0 * (idx + 1) / n)
            print(
                f"  {pct:5.1f}%  ({idx + 1}/{n})  {time.perf_counter() - t_start:.1f}s elapsed",
                flush=True,
            )
    events = contact_events(pairs, pair_times, step_ms) if pose else []
    summary = summarise(frames, events, step_ms)
    kept = [o for o in summary if sum(b - a for a, b in o["seen_spans"]) >= 1000.0 * MIN_SEEN_S]
    keep_ids = {o["id"] for o in kept}
    return {
        "schema": SCHEMA,
        "source_video": f"video/{video.name}",
        "camera_index": cam,
        "labels": labels,
        "stride": stride,
        "fps": fps,
        # The video's first frame, session elapsed ms: the chart's zero.
        "video_start_ms": round(float(times[0]), 1),
        "pose_used": bool(pose),
        "summary": {
            "objects": kept,
            "brief_objects_left_out": len(summary) - len(kept),
            "min_seen_s": MIN_SEEN_S,
        },
        "contacts": [e for e in events if e["object_id"] in keep_ids],
        "frames": frames,
    }


def write_video(video: Path, result: dict, out_path: Path) -> bool:
    """The video with each object's outline and label; a touched object's
    outline is drawn thicker."""
    import cv2
    from gaze.render import partial_path

    keep = {o["id"] for o in result["summary"]["objects"]}
    by_index = {
        f["frame_index"]: [o for o in f["objects"] if o["id"] in keep] for f in result["frames"]
    }
    touching = [(e["object_id"], e["start_ms"], e["end_ms"]) for e in result["contacts"]]
    times = {f["frame_index"]: f["timestamp_ms"] for f in result["frames"]}
    cap = cv2.VideoCapture(str(video))
    fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
    w, h = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)), int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    partial = partial_path(out_path)
    writer = open_writer(partial, fps, (w, h))
    if writer is None:
        cap.release()
        return False
    shown, now = [], 0.0
    idx = 0
    try:
        while True:
            ok, img = cap.read()
            if not ok:
                break
            if idx in by_index:  # between analysed frames, the last result holds
                shown, now = by_index[idx], times[idx]
            for o in shown:
                poly = np.asarray(o.get("polygon") or [], np.int32)
                if len(poly) < 3:
                    continue
                colour = COLOURS[o["id"] % len(COLOURS)]
                held = any(oid == o["id"] and a <= now <= b for oid, a, b in touching)
                cv2.polylines(img, [poly.reshape(-1, 1, 2)], True, colour, 5 if held else 2)
                x, y = poly[:, 0].min(), max(14, poly[:, 1].min() - 6)
                text = f"{o['label']} {o['id']}" + (" touched" if held else "")
                cv2.putText(img, text, (int(x), int(y)), cv2.FONT_HERSHEY_SIMPLEX, 0.6, colour, 2)
            writer.write(img)
            idx += 1
    finally:
        cap.release()
        writer.release()
    publish_video(partial, out_path)
    return True


def main(argv=None, track=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    args = parse_args(argv)
    session = Path(args.session)
    labels = parse_labels(args.objects)
    if not labels:
        print(f"{TAG} Name at least one object to look for.", file=sys.stderr)
        return 1
    weights = Path(args.weights)
    if track is None:
        if not weights.exists():
            print(
                f"{TAG} SAM 3 weights not found ({weights.resolve()}). Download sam3.pt from "
                "Hugging Face (facebook/sam3) after accepting Meta's SAM licence, and put it in "
                "the MOSAIC folder.",
                file=sys.stderr,
            )
            return 1
        from objects.track import track_video as track
    videos = sorted((session / "video").glob("video_*.mp4"))
    if args.camera is not None:
        videos = [v for v in videos if v.stem == f"video_{args.camera}"]
    if not videos:
        print(f"{TAG} No videos to analyse in {session / 'video'}", file=sys.stderr)
        return 1
    out_dir = session / "objects"
    out_dir.mkdir(exist_ok=True)
    failed = 0
    for pos, video in enumerate(videos, start=1):
        cam = int(video.stem.split("_")[1])
        log(f"Camera {pos}/{len(videos)}: {video.name}, looking for {', '.join(labels)}")
        try:
            res = analyse_camera(session, video, cam, labels, weights, max(1, args.stride), track)
            if not args.no_video:
                out_video = out_dir / f"{video.stem}.objects.mp4"
                if write_video(video, res, out_video):
                    res["annotated_video"] = f"objects/{out_video.name}"
            frames = res.pop("frames")
            (out_dir / f"{video.stem}.objects.frames.json").write_text(
                json.dumps({"schema": SCHEMA + "-frames", "frames": frames}), encoding="utf-8"
            )
            res["frames_file"] = f"objects/{video.stem}.objects.frames.json"
            path = out_dir / f"{video.stem}.objects.json"
            path.write_text(json.dumps(res, indent=1), encoding="utf-8")
        except Exception:  # one camera's failure must not stop the others
            traceback.print_exc()
            log(f"{video.name}: failed (see the error above)")
            failed += 1
            continue
        objs = res["summary"]["objects"]
        brief = res["summary"]["brief_objects_left_out"]
        log(
            f"{video.name}: {len(objs)} object(s), {len(res['contacts'])} contact(s)"
            + (f", {brief} seen under {MIN_SEEN_S:g} s left out" if brief else "")
            + ("" if res["pose_used"] else " (no Pose result for this camera: no contacts)")
            + f" -> {path}"
        )
    print(f"  100.0%  ({len(videos)}/{len(videos)})", flush=True)
    return 1 if failed == len(videos) else 0


if __name__ == "__main__":
    sys.exit(main())
