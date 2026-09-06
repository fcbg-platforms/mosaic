"""
MOSAIC pose estimation runner.

Two operating modes:

1. **Session mode** (post-processing — most common):
   Processes all .mp4 files in a recorded session directory and writes
   per-video JSON + CSV keypoint files alongside the originals.

       python analysis/run_pose.py --session /path/to/session_2026-06-07_14-30

2. **Pipe mode** (real-time — launched by MOSAIC internally):
   Reads base64-encoded JPEG frames from stdin, one per line,
   and writes JSON keypoint objects to stdout, one per line.
   MOSAIC uses this mode when "Auto-analyse" is enabled in the UI.

       python analysis/run_pose.py --pipe --model yolov8n-pose.pt

See analysis/README.rst for full documentation.
"""

from __future__ import annotations

import argparse
import base64
import json
import re
import sys
import time
from pathlib import Path

import cv2
import numpy as np
from pose.depth_estimator import DepthEstimator
from pose.human_pose import TRACKER_NAME, HumanPoseEstimator
from pose.keypoints import COCO_KEYPOINTS, COCO_SKELETON, PoseResult

# ── CLI ───────────────────────────────────────────────────────────────────────


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MOSAIC pose estimation")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--session", metavar="DIR", help="Process all .mp4 files in a recorded session directory"
    )
    group.add_argument(
        "--pipe",
        action="store_true",
        help="Pipe mode: read JPEG frames from stdin, write JSON to stdout",
    )
    group.add_argument("--video", metavar="FILE", help="Process a single video file")

    parser.add_argument(
        "--model",
        default="yolov8n-pose.pt",
        help="YOLOv8-pose weights, or a YOLO26-depth checkpoint "
        "(e.g. yolo26n-depth.pt) for a colorized depth-map video "
        "instead of keypoints (default: yolov8n-pose.pt)",
    )
    parser.add_argument(
        "--device",
        default=None,
        help="Inference device: cpu / cuda:0 / mps (auto-detected if omitted)",
    )
    parser.add_argument(
        "--conf", type=float, default=0.40, help="Detection confidence threshold (default: 0.40)"
    )
    parser.add_argument(
        "--skip", type=int, default=1, help="Process every Nth frame (default: 1 = every frame)"
    )
    parser.add_argument(
        "--out-format",
        choices=["json", "csv", "hdf5"],
        default="json",
        help="Output format for session/video mode (default: json)",
    )
    return parser.parse_args()


# ── Session mode ─────────────────────────────────────────────────────────────


def _camera_index_from_filename(video_path: Path) -> int:
    """Parses the real camera index from a MOSAIC-recorded video's own
    filename (e.g. "video_2.mp4" -> 2), instead of trusting a video list's
    positional loop index. VideoManager preserves gaps in camera indices
    when an earlier camera fails to open during recording (see
    VideoManager::open()'s configIndex handling) — e.g. only video_0.mp4
    and video_2.mp4 exist if camera 1 failed — so a plain enumerate() over
    a sorted file list would silently assign the wrong index to video_2.mp4
    (1 instead of 2), mismatching its timestamp CSV lookup and writing the
    wrong camera_index into its .pose.json (later read by run_pose3d.py to
    pick per-camera calibration). Falls back to 0 with a stderr warning if
    the filename doesn't match the expected "video_N.<ext>" pattern.
    """
    m = re.search(r"video_(\d+)", video_path.stem)
    if not m:
        print(
            f"[run_pose] Warning: could not parse a camera index from "
            f"{video_path.name}, defaulting to 0",
            file=sys.stderr,
        )
        return 0
    return int(m.group(1))


def process_session(
    session_dir: Path,
    estimator: HumanPoseEstimator,
    model_name: str,
    skip: int = 1,
    out_format: str = "json",
) -> None:
    videos = sorted((session_dir / "video").glob("*.mp4"))
    if not videos:
        print(f"[run_pose] No .mp4 files found in {session_dir / 'video'}", file=sys.stderr)
        sys.exit(1)

    # Own subfolder, not beside the source video — keeps the video/ folder
    # from being cluttered with per-camera analysis sidecars (mirrors
    # run_face_mask.py's dedicated anonymized/ folder, just per-video
    # instead of a full alternate copy).
    pose_dir = session_dir / "pose"

    print(f"[run_pose] Found {len(videos)} video(s) in {session_dir / 'video'}", flush=True)
    for position, video_path in enumerate(videos, start=1):
        camera_index = _camera_index_from_filename(video_path)
        # Distinct, easily-regexed format ("Camera N/M:") — parsed by
        # AnalysisTabW's camera-progress bar, separate from the per-frame
        # "NN.N% (a/b)" progress lines process_video() prints below. Uses
        # `position` (this video's place in the loop), not `camera_index`
        # (which can have gaps) — the progress bar cares about "how many of
        # the M videos in this run have we processed," not the raw index.
        print(f"[run_pose] Camera {position}/{len(videos)}: {video_path.name}", flush=True)
        process_video(
            video_path,
            estimator,
            model_name,
            skip=skip,
            out_format=out_format,
            camera_index=camera_index,
            output_dir=pose_dir,
        )


def process_video(
    video_path: Path,
    estimator: HumanPoseEstimator,
    model_name: str,
    skip: int = 1,
    out_format: str = "json",
    camera_index: int = 0,
    output_dir: Path | None = None,
) -> None:
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        print(f"[run_pose] Cannot open {video_path}", file=sys.stderr)
        return

    # One video = one identity space. The estimator carries tracker state
    # across calls (persist=True), and process_session() threads a single
    # estimator through every camera in the session, so without this reset
    # camera N's live tracks would be matched against camera N+1's opening
    # frames. Placed here rather than in process_session()'s loop because
    # process_video() *is* the boundary — that also covers --video mode and
    # makes it impossible for a future caller to forget. run_pipe()
    # deliberately does not reset: it is one continuous stream.
    estimator.reset_tracker()

    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    print(f"[run_pose] Processing {video_path.name}  ({total} frames @ {fps:.1f} fps)", flush=True)

    # Scales to the video's own length instead of a fixed 100-frame stride:
    # short session videos (~141 frames is typical) get an update every
    # frame, while long videos are capped at ~200 total prints. These lines
    # are consumed by AnalysisTabW's progress-bar regex, not dumped into the
    # visible log, so there's no "avoid spamming the log" reason to keep the
    # interval coarse.
    progress_interval = max(1, total // 200)

    if skip > 1:
        # Worth saying out loud now that identity is tracked: the tracker only
        # sees the frames we actually run inference on, so people move `skip`x
        # further between its updates, and its lost-track buffer (counted in
        # updates, not video frames) stays open `skip`x longer in real time.
        print(
            f"[run_pose] --skip {skip}: subject identity will be less reliable "
            f"(the tracker sees only every {skip}th frame)",
            file=sys.stderr,
            flush=True,
        )

    # Load matching timestamp CSV if present (writes absolute timestamps per
    # frame). Built directly from camera_index rather than substituting
    # "video" -> "timestamps_cam" in the video's own stem: video files are
    # named "video_N.mp4" (underscore before the index) but timestamp files
    # are named "timestamps_camN.csv" (no underscore) — the naive stem
    # substring-replace produced "timestamps_cam_N.csv" (wrong, with a
    # stray underscore), which never matched any real file, silently
    # leaving every frame's timestamp at 0 and collapsing the entire
    # Analysis-tab kinematics chart onto a single point at ms=0.
    ts_csv = video_path.with_name(f"timestamps_cam{camera_index}.csv")
    timestamps: list[int] = []
    if ts_csv.exists():
        import csv

        with ts_csv.open() as f:
            for row in csv.DictReader(f):
                try:
                    timestamps.append(int(row.get("elapsed_ns", 0)))
                except ValueError:
                    timestamps.append(0)

    results: list[dict] = []
    frame_idx = 0
    t_start = time.perf_counter()

    while True:
        ok, frame = cap.read()
        if not ok:
            break

        if frame_idx % skip == 0:
            ts_ns = timestamps[frame_idx] if frame_idx < len(timestamps) else 0
            result = estimator.infer(
                frame, frame_index=frame_idx, timestamp_ns=ts_ns, camera_index=camera_index
            )
            results.append(_result_to_dict(result))

        frame_idx += 1
        if frame_idx % progress_interval == 0:
            elapsed = time.perf_counter() - t_start
            pct = frame_idx / max(total, 1) * 100
            print(f"  {pct:5.1f}%  ({frame_idx}/{total})  {elapsed:.1f}s elapsed", flush=True)

    cap.release()
    elapsed = time.perf_counter() - t_start
    print(
        f"[run_pose] Done. {frame_idx} frames in {elapsed:.1f}s "
        f"({frame_idx/elapsed:.1f} fps throughput)",
        flush=True,
    )

    _write_results(video_path, results, out_format, model_name, output_dir)


def _write_results(
    video_path: Path,
    results: list[dict],
    out_format: str,
    model_name: str,
    output_dir: Path | None = None,
) -> None:
    # output_dir is only given by process_session() (real session-folder
    # runs) — the standalone `--video FILE` CLI mode has no session/pose-
    # folder concept to hang a subfolder off of, so it keeps writing beside
    # the source video, exactly as before.
    #
    # base_name is namespaced by which model produced it (e.g.
    # "video_0.yolov8n-pose") so a second model run against the same video
    # no longer silently overwrites the first — mirrors AnalysisTabW::
    # slug_for_model() exactly (strip the trailing ".pt"). Deliberately NOT
    # built via Path.with_suffix() below: that method only understands a
    # single trailing dot-segment as "the suffix" — once base_name itself
    # contains a dot (from the model slug), with_suffix(".pose.json") would
    # silently eat the slug back off instead of appending. Every branch
    # below builds its full filename as a plain string instead.
    model_slug = re.sub(r"\.pt$", "", model_name)
    base_name = f"{video_path.stem}.{model_slug}"
    out_dir = output_dir if output_dir is not None else video_path.parent
    if output_dir is not None:
        output_dir.mkdir(parents=True, exist_ok=True)
    if out_format == "json":
        out_path = out_dir / f"{base_name}.pose.json"
        out_path.write_text(
            json.dumps(
                {
                    "source_video": video_path.name,
                    "model": model_name,
                    # Names the cross-frame tracker behind subject_id. Absent
                    # in files written before tracking existed, whose
                    # subject_id is merely per-frame detection order — the C++
                    # reader keys its "identity is/isn't tracked" wording off
                    # exactly this rather than inferring it from the ids.
                    "tracker": TRACKER_NAME,
                    "keypoint_names": COCO_KEYPOINTS,
                    "skeleton_edges": COCO_SKELETON,
                    "frames": results,
                },
                indent=2,
            )
        )
        print(f"[run_pose] Keypoints → {out_path}", flush=True)

    elif out_format == "csv":
        import csv as csv_mod

        out_path = out_dir / f"{base_name}.pose.csv"
        with out_path.open("w", newline="") as f:
            writer = csv_mod.writer(f)
            kp_headers = [f"{kp}_x,{kp}_y,{kp}_vis".split(",") for kp in COCO_KEYPOINTS]
            flat_headers = ["frame", "timestamp_ns", "subject_id", "confidence"]
            for h in kp_headers:
                flat_headers.extend(h)
            writer.writerow(flat_headers)
            for frame in results:
                for subj in frame.get("subjects", []):
                    row = [
                        frame["frame_index"],
                        frame["timestamp_ns"],
                        subj["subject_id"],
                        subj["confidence"],
                    ]
                    for (kx, ky), vis in zip(subj["keypoints"], subj["visibilities"], strict=False):
                        row.extend([kx, ky, vis])
                    writer.writerow(row)
        print(f"[run_pose] Keypoints → {out_path}", flush=True)

    elif out_format == "hdf5":
        import h5py

        out_path = out_dir / f"{base_name}.pose.h5"
        with h5py.File(out_path, "w") as hf:
            hf.attrs["source_video"] = video_path.name
            hf.attrs["keypoint_names"] = COCO_KEYPOINTS
            # Flatten to array for efficient storage
            frame_ids = np.array([r["frame_index"] for r in results])
            hf.create_dataset("frame_index", data=frame_ids)
        print(f"[run_pose] Keypoints → {out_path}", flush=True)


# ── Depth mode ────────────────────────────────────────────────────────────────
#
# A `--model` value containing "depth" (e.g. "yolo26n-depth.pt") selects a
# completely different output shape from pose estimation — a dense per-pixel
# depth map, not keypoints — so it gets its own session/video processing
# functions rather than reusing process_session()/process_video() above.
# Mirrors run_face_mask.py's "write a processed video into its own session
# subfolder" pattern (here: depth/ instead of anonymized/) rather than
# writing a JSON sidecar, since there's no keypoint/skeleton data to record.


def process_session_depth(
    session_dir: Path, estimator: DepthEstimator, model_name: str, skip: int = 1
) -> None:
    videos = sorted((session_dir / "video").glob("*.mp4"))
    if not videos:
        print(f"[run_pose] No .mp4 files found in {session_dir / 'video'}", file=sys.stderr)
        sys.exit(1)

    depth_dir = session_dir / "depth"
    print(f"[run_pose] Found {len(videos)} video(s) in {session_dir / 'video'}", flush=True)
    for position, video_path in enumerate(videos, start=1):
        # Same "Camera N/M:" banner format run_pose.py's own keypoint path
        # prints above — AnalysisTabW's camera-progress bar regex matches
        # any "[run_X] Camera N/M:" line, not just this specific call site.
        print(f"[run_pose] Camera {position}/{len(videos)}: {video_path.name}", flush=True)
        process_video_depth(video_path, estimator, model_name, skip=skip, output_dir=depth_dir)


def process_video_depth(
    video_path: Path,
    estimator: DepthEstimator,
    model_name: str,
    skip: int = 1,
    output_dir: Path | None = None,
) -> None:
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        print(f"[run_pose] Cannot open {video_path}", file=sys.stderr)
        return

    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    print(f"[run_pose] Processing {video_path.name}  ({total} frames @ {fps:.1f} fps)", flush=True)

    output_dir = output_dir or video_path.parent
    output_dir.mkdir(parents=True, exist_ok=True)
    # Namespaced by which depth model produced it — mirrors _write_results()'s
    # keypoint-path treatment above, since the 3 depth variants (n/s/m)
    # shared the identical silent-overwrite gap.
    model_slug = re.sub(r"\.pt$", "", model_name)
    out_path = output_dir / f"{video_path.stem}.{model_slug}{video_path.suffix}"

    writer = _open_video_writer(out_path, fps, (width, height))
    if writer is None:
        print(f"[run_pose] Could not open a video writer for {out_path}", file=sys.stderr)
        cap.release()
        return

    # Same scales-to-video-length progress cadence as process_video() above.
    progress_interval = max(1, total // 200)

    frame_idx = 0
    last_colorized: np.ndarray | None = None
    t_start = time.perf_counter()

    while True:
        ok, frame = cap.read()
        if not ok:
            break

        # A single frame's depth inference failing shouldn't abort the whole
        # video (or, since process_session_depth() calls this once per
        # camera, the other cameras in the session) — fall back to writing
        # the original frame unprocessed rather than a blacked-out one (no
        # privacy concern here, unlike run_face_mask.py's equivalent guard).
        try:
            if frame_idx % skip == 0:
                last_colorized = estimator.infer_colorized(frame)
            writer.write(last_colorized if last_colorized is not None else frame)
        except Exception as exc:  # noqa: BLE001
            print(
                f"[run_pose] {video_path.name}: frame {frame_idx} failed ({exc}) "
                f"— writing the original frame instead",
                file=sys.stderr,
                flush=True,
            )
            writer.write(frame)

        frame_idx += 1
        if frame_idx % progress_interval == 0:
            elapsed = time.perf_counter() - t_start
            pct = frame_idx / max(total, 1) * 100
            print(f"  {pct:5.1f}%  ({frame_idx}/{total})  {elapsed:.1f}s elapsed", flush=True)

    cap.release()
    writer.release()

    if frame_idx == 0:
        print(
            f"[run_pose] {video_path.name}: 0 frames read — removing empty output", file=sys.stderr
        )
        out_path.unlink(missing_ok=True)
        return

    elapsed = time.perf_counter() - t_start
    print(
        f"[run_pose] Done. {frame_idx} frames in {elapsed:.1f}s "
        f"({frame_idx/elapsed:.1f} fps throughput) → {out_path}",
        flush=True,
    )


def _open_video_writer(out_path: Path, fps: float, size: tuple[int, int]):
    # Same avc1-then-mp4v fallback as run_face_mask.py's _open_writer() —
    # avc1 (H.264) plays back more reliably in Qt/Windows Media Foundation
    # than mp4v, but isn't always available depending on the OpenCV build's
    # bundled FFmpeg.
    for fourcc_name in ("avc1", "mp4v"):
        fourcc = cv2.VideoWriter_fourcc(*fourcc_name)
        writer = cv2.VideoWriter(str(out_path), fourcc, fps, size)
        if writer.isOpened():
            return writer
        writer.release()
    return None


# ── Pipe mode ─────────────────────────────────────────────────────────────────


def run_pipe(estimator: HumanPoseEstimator) -> None:
    """
    Reads base64-encoded JPEG frames from stdin (one per line).
    Writes JSON results to stdout (one per line), flushed immediately.

    Protocol (used by MOSAIC's AnalysisManager):
      stdin:  <base64-jpeg>\\n
      stdout: <json-result>\\n
    """
    print(json.dumps({"status": "ready", "backend": "yolov8-pose"}), flush=True)

    frame_idx = 0
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            img_bytes = base64.b64decode(line)
            nparr = np.frombuffer(img_bytes, np.uint8)
            frame = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
            if frame is None:
                print(json.dumps({"error": "decode_failed", "frame": frame_idx}), flush=True)
                frame_idx += 1
                continue

            result = estimator.infer(frame, frame_index=frame_idx)
            print(json.dumps(_result_to_dict(result)), flush=True)
        except Exception as exc:  # noqa: BLE001
            print(json.dumps({"error": str(exc), "frame": frame_idx}), flush=True)
        frame_idx += 1


# ── Utilities ─────────────────────────────────────────────────────────────────


def _result_to_dict(result: PoseResult) -> dict:
    return {
        "frame_index": result.frame_index,
        "timestamp_ns": result.timestamp_ns,
        "camera_index": result.camera_index,
        "inference_ms": round(result.inference_ms, 2),
        "backend": result.backend,
        "subjects": [
            {
                "subject_id": s.subject_id,
                "confidence": round(s.confidence, 3),
                "bbox_xyxy": [round(v, 1) for v in s.bbox_xyxy],
                "keypoints": [[round(x, 1), round(y, 1)] for x, y in s.keypoints],
                "visibilities": [round(v, 3) for v in s.visibilities],
            }
            for s in result.subjects
        ],
    }


# ── Entry point ───────────────────────────────────────────────────────────────


def main() -> None:
    args = parse_args()

    # A depth checkpoint (e.g. "yolo26n-depth.pt") is a different task shape
    # entirely (dense per-pixel depth, no keypoints) — dispatched here,
    # before HumanPoseEstimator is ever constructed, so the two paths never
    # share an estimator instance/type.
    if "depth" in args.model:
        depth_estimator = DepthEstimator(model_name=args.model, device=args.device)
        if args.session:
            process_session_depth(Path(args.session), depth_estimator, args.model, skip=args.skip)
        elif args.video:
            video_path = Path(args.video)
            # Same "write beside the session's video/ folder, into a sibling
            # depth/ folder" convention run_face_mask.py's --video mode uses
            # for anonymized/, so AnalysisTabW's depth_video_path_for()
            # lookup finds it either way this script was invoked.
            session_root = (
                video_path.parent.parent if video_path.parent.name == "video" else video_path.parent
            )
            process_video_depth(
                video_path,
                depth_estimator,
                args.model,
                skip=args.skip,
                output_dir=session_root / "depth",
            )
        elif args.pipe:
            print("[run_pose] --pipe mode is not supported for depth models", file=sys.stderr)
            sys.exit(1)
        return

    estimator = HumanPoseEstimator(
        model_name=args.model,
        device=args.device,
        conf_threshold=args.conf,
    )

    if args.pipe:
        run_pipe(estimator)
    elif args.session:
        process_session(
            Path(args.session), estimator, args.model, skip=args.skip, out_format=args.out_format
        )
    elif args.video:
        video_path = Path(args.video)
        # Same real-filename-based index used by process_session() — the
        # previous default of camera_index=0 silently loaded camera 0's
        # timestamp CSV (if one happened to exist alongside a *different*
        # camera's video) instead of the correct one, or the correct one
        # not at all.
        process_video(
            video_path,
            estimator,
            args.model,
            skip=args.skip,
            out_format=args.out_format,
            camera_index=_camera_index_from_filename(video_path),
        )


if __name__ == "__main__":
    main()
