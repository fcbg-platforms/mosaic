"""
MOSAIC Hands: 21 points per hand (wrist, each finger joint and fingertip) on
each camera, with whose hand it is and which side, from the camera's Pose
result. Made for close-up cameras on a table, where hands are large enough
to see the fingers.

    python analysis/run_hands.py --session DIR [--model rtmw|mediapipe]
        [--camera N ...] [--stride 1] [--no-video]

Writes, per camera, in ``<session>/hands/``:

* ``video_N.hands.json``: the model, and a summary per hand (person, side,
  how long it was seen and when, its size in pixels, its mean score);
* ``video_N.hands.frames.json``: every analysed frame's hands: person,
  side, score, the 21 points (pixels) and their scores;
* ``video_N.hands.mp4``: the video with the hands drawn on it, the
  person's left hand in green and right in orange.
See docs/math/hands.rst.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import traceback
from pathlib import Path

import numpy as np
from camera_pick import add_camera_arg, pick_cameras
from hands.people import EDGES, KEYPOINT_NAMES, summarise
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
    print(f"  100.0%  ({len(videos)}/{len(videos)})", flush=True)
    return 1 if failed == len(videos) else 0


if __name__ == "__main__":
    sys.exit(main())
