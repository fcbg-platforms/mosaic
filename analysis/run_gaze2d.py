"""
MOSAIC calibration-free 2D gaze analysis runner — extracts a per-frame,
per-camera normalized gaze-offset heuristic (gaze2d.Gaze2dEstimator, the
same iris-offset math already running live in the Real-time tab and in the
Multi-Camera Gaze Fusion plugin) with no camera intrinsic or room/extrinsic
calibration prerequisite at all. In --session mode, writes one
"<name>.gaze2d.json" per camera into the session's own gaze2d/ subfolder;
in standalone --video mode, writes it beside the source video. Originals
are never modified.

    python analysis/run_gaze2d.py --session /path/to/session_2026-06-07_14-30
    python analysis/run_gaze2d.py --video /path/to/video/video_0.mp4

Unlike analysis/run_rppg.py, --skip IS offered here: this plugin has no
Nyquist/frequency-analysis constraint (a plain per-frame heuristic, not a
sliding-window signal-processing pass), so it follows Pose's/Expression's
established skip-with-nearest-frame-fallback convention instead.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

import cv2
from gaze2d.estimator import Gaze2dEstimator, gaze_on_target

# ── CLI ───────────────────────────────────────────────────────────────────────


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="MOSAIC calibration-free 2D gaze analysis (no camera "
        "intrinsic/extrinsic calibration required)"
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--session", metavar="DIR", help="Process all .mp4 files in a recorded session directory"
    )
    group.add_argument("--video", metavar="FILE", help="Process a single video file")

    parser.add_argument(
        "--min-confidence",
        type=float,
        default=0.5,
        help="Face detection/presence/tracking confidence threshold (default: 0.5)",
    )
    parser.add_argument(
        "--skip", type=int, default=1, help="Process every Nth frame (default: 1 = every frame)"
    )
    return parser.parse_args()


# ── Session mode ─────────────────────────────────────────────────────────────


def _camera_index_from_filename(video_path: Path) -> int:
    """Parses the real camera index from a MOSAIC-recorded video's own
    filename (e.g. "video_2.mp4" -> 2) — mirrors run_pose.py's/
    run_expression.py's/run_rppg.py's identical helper/rationale
    (VideoManager preserves gaps in camera indices when an earlier camera
    fails to open during recording, so a plain enumerate() over a sorted
    file list can silently assign the wrong index). Falls back to 0 with a
    stderr warning if the filename doesn't match "video_N.<ext>"."""
    m = re.search(r"video_(\d+)", video_path.stem)
    if not m:
        print(
            f"[run_gaze2d] Warning: could not parse a camera index from "
            f"{video_path.name}, defaulting to 0",
            file=sys.stderr,
        )
        return 0
    return int(m.group(1))


def process_session(
    session_dir: Path,
    min_confidence: float,
    skip: int,
) -> None:
    videos = sorted((session_dir / "video").glob("*.mp4"))
    if not videos:
        print(f"[run_gaze2d] No .mp4 files found in {session_dir / 'video'}", file=sys.stderr)
        sys.exit(1)

    print(f"[run_gaze2d] Found {len(videos)} video(s) in {session_dir / 'video'}", flush=True)

    # Own subfolder, mirrors run_pose.py's pose_dir / run_expression.py's
    # expression_dir / run_rppg.py's rppg_dir convention. Forward-only:
    # sessions analyzed before this feature existed have no gaze2d/ folder
    # and won't retroactively gain one (this project's established
    # no-migration convention).
    gaze2d_dir = session_dir / "gaze2d"

    # Built once, reused across every video in the session — avoids the
    # redundant-model-reload mistake item 17's diarization plugin had to
    # fix after code review.
    estimator = Gaze2dEstimator(min_confidence=min_confidence)

    for position, video_path in enumerate(videos, start=1):
        camera_index = _camera_index_from_filename(video_path)
        print(f"[run_gaze2d] Camera {position}/{len(videos)}: {video_path.name}", flush=True)
        process_video(
            video_path,
            estimator,
            skip,
            camera_index=camera_index,
            output_dir=gaze2d_dir,
            min_confidence=min_confidence,
        )


def _read_timestamps_ms(video_path: Path, camera_index: int) -> list[float]:
    # Built directly from camera_index (not a naive "video" -> "timestamps_cam"
    # substring-replace on the video's own stem) — that substitution turns
    # "video_0" into "timestamps_cam_0.csv" (stray underscore), which never
    # matches the real "timestamps_cam0.csv" file. Same bug already found
    # and fixed in run_pose.py/run_pose3d.py/run_gaze_fusion.py/
    # run_expression.py/run_rppg.py this session.
    ts_csv = video_path.with_name(f"timestamps_cam{camera_index}.csv")
    timestamps_ms: list[float] = []
    if ts_csv.exists():
        import csv

        with ts_csv.open() as f:
            for row in csv.DictReader(f):
                try:
                    timestamps_ms.append(int(row.get("elapsed_ns", 0)) / 1e6)
                except ValueError:
                    timestamps_ms.append(0.0)
    return timestamps_ms


def process_video(
    video_path: Path,
    estimator: Gaze2dEstimator,
    skip: int = 1,
    camera_index: int = 0,
    output_dir: Path | None = None,
    min_confidence: float = 0.5,
) -> None:
    skip = max(1, skip)  # --skip 0 would divide-by-zero on frame_idx % skip below

    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        print(f"[run_gaze2d] Cannot open {video_path}", file=sys.stderr)
        return

    fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    print(
        f"[run_gaze2d] Processing {video_path.name}  ({total} frames @ {fps:.1f} fps)",
        flush=True,
    )

    # Scales to the video's own length instead of a fixed stride, matching
    # every sibling analysis script's identical fix.
    progress_interval = max(1, total // 200)

    timestamps_ms = _read_timestamps_ms(video_path, camera_index)
    have_real_timestamps = len(timestamps_ms) > 0

    frames: list[dict] = []
    on_target_count = 0
    with_face_count = 0
    dx_sum = 0.0
    dy_sum = 0.0

    frame_idx = 0
    t_start = time.perf_counter()
    while True:
        ok, frame = cap.read()
        if not ok:
            break

        if have_real_timestamps:
            ts_ms = (
                timestamps_ms[frame_idx] if frame_idx < len(timestamps_ms) else timestamps_ms[-1]
            )
        else:
            ts_ms = frame_idx / fps * 1000.0

        # Frames are NOT emitted on skipped indices — gaps are handled by
        # nearest_frame(), matching run_expression.py's identical skip
        # convention (no "reuse last detection" fallback needed here; that
        # concern only applies to a video-*rewriting* pass like
        # run_face_mask.py, which doesn't apply here).
        if frame_idx % skip == 0:
            sample = estimator.estimate(frame)
            if sample is not None:
                frames.append(
                    {
                        "frame_index": frame_idx,
                        "timestamp_ms": round(ts_ms),
                        "face_detected": True,
                        "face_box_px": [round(v) for v in sample.bbox_xyxy],
                        "gaze_dx": round(sample.gaze_dx, 4),
                        "gaze_dy": round(sample.gaze_dy, 4),
                    }
                )
                with_face_count += 1
                dx_sum += sample.gaze_dx
                dy_sum += sample.gaze_dy
                if gaze_on_target(sample.gaze_dx, sample.gaze_dy):
                    on_target_count += 1
            else:
                frames.append(
                    {
                        "frame_index": frame_idx,
                        "timestamp_ms": round(ts_ms),
                        "face_detected": False,
                        "face_box_px": None,
                        "gaze_dx": None,
                        "gaze_dy": None,
                    }
                )

        frame_idx += 1
        if frame_idx % progress_interval == 0:
            elapsed = time.perf_counter() - t_start
            pct = frame_idx / max(total, 1) * 100
            print(f"  {pct:5.1f}%  ({frame_idx}/{total})  {elapsed:.1f}s elapsed", flush=True)

    cap.release()
    elapsed = time.perf_counter() - t_start
    print(
        f"[run_gaze2d] Done. {frame_idx} frames in {elapsed:.1f}s "
        f"({frame_idx / max(elapsed, 1e-9):.1f} fps throughput)",
        flush=True,
    )

    analyzed = len(frames)
    summary = {
        "pct_frames_with_face": round(with_face_count / analyzed, 3) if analyzed else 0.0,
        "mean_gaze_dx": round(dx_sum / with_face_count, 4) if with_face_count else None,
        "mean_gaze_dy": round(dy_sum / with_face_count, 4) if with_face_count else None,
        "pct_on_target": round(on_target_count / with_face_count, 3) if with_face_count else None,
    }

    _write_results(video_path, frames, summary, camera_index, min_confidence, output_dir=output_dir)


def _write_results(
    video_path: Path,
    frames: list[dict],
    summary: dict,
    camera_index: int,
    min_confidence: float,
    output_dir: Path | None = None,
) -> None:
    if output_dir is not None:
        output_dir.mkdir(parents=True, exist_ok=True)
        out_path = output_dir / f"{video_path.stem}.gaze2d.json"
    else:
        # Standalone --video CLI mode has no session/gaze2d-folder concept
        # to hang a subfolder off — beside-the-video, matching sibling
        # scripts' identical fallback.
        out_path = video_path.with_name(f"{video_path.stem}.gaze2d.json")

    doc = {
        "schema": "mosaic-gaze2d-v1",
        "source_video": video_path.name,
        "camera_index": camera_index,
        "min_confidence": min_confidence,
        "frames": frames,
        "summary": summary,
    }
    out_path.write_text(json.dumps(doc, indent=2))
    print(f"[run_gaze2d] Gaze data → {out_path}", flush=True)


# ── Entry point ───────────────────────────────────────────────────────────────


def main() -> None:
    args = parse_args()

    if args.session:
        process_session(Path(args.session), args.min_confidence, args.skip)
    elif args.video:
        estimator = Gaze2dEstimator(min_confidence=args.min_confidence)
        video_path = Path(args.video)
        process_video(
            video_path,
            estimator,
            args.skip,
            camera_index=_camera_index_from_filename(video_path),
            min_confidence=args.min_confidence,
        )


if __name__ == "__main__":
    main()
