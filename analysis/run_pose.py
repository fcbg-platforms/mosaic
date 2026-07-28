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
import sys
import time
from dataclasses import asdict
from pathlib import Path
from typing import Optional

import cv2
import numpy as np

from pose.human_pose import HumanPoseEstimator
from pose.keypoints import COCO_KEYPOINTS, COCO_SKELETON, PoseResult

# ── CLI ───────────────────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MOSAIC pose estimation")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--session",  metavar="DIR",
                       help="Process all .mp4 files in a recorded session directory")
    group.add_argument("--pipe",     action="store_true",
                       help="Pipe mode: read JPEG frames from stdin, write JSON to stdout")
    group.add_argument("--video",    metavar="FILE",
                       help="Process a single video file")

    parser.add_argument("--model",   default="yolov8n-pose.pt",
                        help="YOLOv8 model weights (default: yolov8n-pose.pt)")
    parser.add_argument("--device",  default=None,
                        help="Inference device: cpu / cuda:0 / mps (auto-detected if omitted)")
    parser.add_argument("--conf",    type=float, default=0.40,
                        help="Detection confidence threshold (default: 0.40)")
    parser.add_argument("--skip",    type=int,   default=1,
                        help="Process every Nth frame (default: 1 = every frame)")
    parser.add_argument("--out-format", choices=["json", "csv", "hdf5"], default="json",
                        help="Output format for session/video mode (default: json)")
    return parser.parse_args()

# ── Session mode ─────────────────────────────────────────────────────────────

def process_session(session_dir: Path, estimator: HumanPoseEstimator,
                    skip: int = 1, out_format: str = "json") -> None:
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
    for camera_index, video_path in enumerate(videos):
        # Distinct, easily-regexed format ("Camera N/M:") — parsed by
        # AnalysisTabW's camera-progress bar, separate from the per-frame
        # "NN.N% (a/b)" progress lines process_video() prints below.
        print(f"[run_pose] Camera {camera_index + 1}/{len(videos)}: {video_path.name}", flush=True)
        process_video(video_path, estimator, skip=skip, out_format=out_format,
                      camera_index=camera_index, output_dir=pose_dir)

def process_video(video_path: Path, estimator: HumanPoseEstimator,
                  skip: int = 1, out_format: str = "json",
                  camera_index: int = 0,
                  output_dir: Optional[Path] = None) -> None:
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        print(f"[run_pose] Cannot open {video_path}", file=sys.stderr)
        return

    fps   = cap.get(cv2.CAP_PROP_FPS) or 30.0
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    print(f"[run_pose] Processing {video_path.name}  ({total} frames @ {fps:.1f} fps)", flush=True)

    # Scales to the video's own length instead of a fixed 100-frame stride:
    # short session videos (~141 frames is typical) get an update every
    # frame, while long videos are capped at ~200 total prints. These lines
    # are consumed by AnalysisTabW's progress-bar regex, not dumped into the
    # visible log, so there's no "avoid spamming the log" reason to keep the
    # interval coarse.
    progress_interval = max(1, total // 200)

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
    t_start   = time.perf_counter()

    while True:
        ok, frame = cap.read()
        if not ok:
            break

        if frame_idx % skip == 0:
            ts_ns = timestamps[frame_idx] if frame_idx < len(timestamps) else 0
            result = estimator.infer(frame, frame_index=frame_idx, timestamp_ns=ts_ns,
                                      camera_index=camera_index)
            results.append(_result_to_dict(result))

        frame_idx += 1
        if frame_idx % progress_interval == 0:
            elapsed = time.perf_counter() - t_start
            pct = frame_idx / max(total, 1) * 100
            print(f"  {pct:5.1f}%  ({frame_idx}/{total})  {elapsed:.1f}s elapsed", flush=True)

    cap.release()
    elapsed = time.perf_counter() - t_start
    print(f"[run_pose] Done. {frame_idx} frames in {elapsed:.1f}s "
          f"({frame_idx/elapsed:.1f} fps throughput)", flush=True)

    _write_results(video_path, results, out_format, output_dir)

def _write_results(video_path: Path, results: list[dict], out_format: str,
                    output_dir: Optional[Path] = None) -> None:
    # output_dir is only given by process_session() (real session-folder
    # runs) — the standalone `--video FILE` CLI mode has no session/pose-
    # folder concept to hang a subfolder off of, so it keeps writing beside
    # the source video, exactly as before.
    if output_dir is not None:
        output_dir.mkdir(parents=True, exist_ok=True)
        base = output_dir / video_path.stem
    else:
        base = video_path.with_suffix("")
    if out_format == "json":
        out_path = base.with_suffix(".pose.json")
        out_path.write_text(json.dumps({
            "source_video": video_path.name,
            "keypoint_names": COCO_KEYPOINTS,
            "skeleton_edges": COCO_SKELETON,
            "frames": results,
        }, indent=2))
        print(f"[run_pose] Keypoints → {out_path}", flush=True)

    elif out_format == "csv":
        import csv as csv_mod
        out_path = base.with_suffix(".pose.csv")
        with out_path.open("w", newline="") as f:
            writer = csv_mod.writer(f)
            kp_headers = [f"{kp}_x,{kp}_y,{kp}_vis".split(",") for kp in COCO_KEYPOINTS]
            flat_headers = ["frame", "timestamp_ns", "subject_id", "confidence"]
            for h in kp_headers:
                flat_headers.extend(h)
            writer.writerow(flat_headers)
            for frame in results:
                for subj in frame.get("subjects", []):
                    row = [frame["frame_index"], frame["timestamp_ns"],
                           subj["subject_id"], subj["confidence"]]
                    for (kx, ky), vis in zip(subj["keypoints"], subj["visibilities"]):
                        row.extend([kx, ky, vis])
                    writer.writerow(row)
        print(f"[run_pose] Keypoints → {out_path}", flush=True)

    elif out_format == "hdf5":
        import h5py
        out_path = base.with_suffix(".pose.h5")
        with h5py.File(out_path, "w") as hf:
            hf.attrs["source_video"]    = video_path.name
            hf.attrs["keypoint_names"]  = COCO_KEYPOINTS
            # Flatten to array for efficient storage
            frame_ids = np.array([r["frame_index"] for r in results])
            hf.create_dataset("frame_index", data=frame_ids)
        print(f"[run_pose] Keypoints → {out_path}", flush=True)

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
            img_bytes  = base64.b64decode(line)
            nparr      = np.frombuffer(img_bytes, np.uint8)
            frame      = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
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
        "frame_index":   result.frame_index,
        "timestamp_ns":  result.timestamp_ns,
        "camera_index":  result.camera_index,
        "inference_ms":  round(result.inference_ms, 2),
        "backend":       result.backend,
        "subjects": [
            {
                "subject_id":  s.subject_id,
                "confidence":  round(s.confidence, 3),
                "bbox_xyxy":   [round(v, 1) for v in s.bbox_xyxy],
                "keypoints":   [[round(x, 1), round(y, 1)] for x, y in s.keypoints],
                "visibilities": [round(v, 3) for v in s.visibilities],
            }
            for s in result.subjects
        ],
    }

# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    args = parse_args()

    estimator = HumanPoseEstimator(
        model_name=args.model,
        device=args.device,
        conf_threshold=args.conf,
    )

    if args.pipe:
        run_pipe(estimator)
    elif args.session:
        process_session(Path(args.session), estimator,
                        skip=args.skip, out_format=args.out_format)
    elif args.video:
        process_video(Path(args.video), estimator,
                      skip=args.skip, out_format=args.out_format)


if __name__ == "__main__":
    main()
