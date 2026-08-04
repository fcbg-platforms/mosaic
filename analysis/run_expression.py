"""
MOSAIC facial-expression analysis runner — detects faces with MediaPipe
FaceLandmarker (blendshapes enabled) and classifies a dominant basic-emotion
label per face, using either a rule-based blendshape heuristic (default) or
a dedicated pretrained FER+ ONNX model. In --session mode, writes one
"<name>.expression.json" per camera into the session's own expression/
subfolder; in standalone --video mode, writes it beside the source video.
Originals are never modified.

    python analysis/run_expression.py --session /path/to/session_2026-06-07_14-30
    python analysis/run_expression.py --video /path/to/video/video_0.mp4 --backend ferplus

See analysis/README.rst for full documentation.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path
from typing import Callable, Optional

import cv2
import numpy as np

from expression.classifier import classify_expression
from expression.detector import (
    BLENDSHAPE_NAMES,
    FaceExpression,
    MediaPipeExpressionDetector,
    crop_bbox,
)

ClassifyFn = Callable[[FaceExpression, np.ndarray], tuple[str, float, Optional[dict[str, float]]]]

# ── CLI ───────────────────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MOSAIC facial expression analysis")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--session", metavar="DIR",
                        help="Process all .mp4 files in a recorded session directory")
    group.add_argument("--video",   metavar="FILE",
                        help="Process a single video file")

    parser.add_argument("--backend", choices=["heuristic", "ferplus", "pyfeat"], default="heuristic",
                         help="Expression classifier: rule-based blendshape heuristic (fast, "
                              "transparent, default), a pretrained FER+ ONNX model (more "
                              "validated, downloads an extra ~34MB model), or py-feat's "
                              "Detectorv1 (adds 20 FACS Action Unit scores alongside the "
                              "emotion label, but is notably slower — roughly 0.1-0.8s/frame "
                              "on CPU)")
    parser.add_argument("--max-faces", type=int, default=5,
                         help="Maximum simultaneous faces to detect (default: 5)")
    parser.add_argument("--min-confidence", type=float, default=0.5,
                         help="Face detection/presence confidence threshold (default: 0.5)")
    parser.add_argument("--skip", type=int, default=1,
                         help="Process every Nth frame (default: 1 = every frame)")
    return parser.parse_args()


def _make_classify_fn(backend: str) -> ClassifyFn:
    if backend == "heuristic":
        return lambda face, frame: (*classify_expression(BLENDSHAPE_NAMES, face.blendshape_scores), None)

    if backend == "ferplus":
        from expression.ferplus import FerPlusClassifier
        ferplus = FerPlusClassifier()   # loads the ONNX session once, reused for every frame

        def _classify(face: FaceExpression, frame: np.ndarray) -> tuple[str, float, None]:
            crop = crop_bbox(frame, face.bbox_xyxy)
            if crop.size == 0:
                return "Neutral", 0.0, None   # degenerate/out-of-frame box — don't crash the run
            return (*ferplus.classify(crop), None)

        return _classify

    if backend == "pyfeat":
        from expression.pyfeat import PyFeatClassifier
        pyfeat = PyFeatClassifier()   # loads Detectorv1 once, reused for every frame

        def _classify(face: FaceExpression, frame: np.ndarray) -> tuple[str, float, dict]:
            crop = crop_bbox(frame, face.bbox_xyxy)
            if crop.size == 0:
                return "Neutral", 0.0, {}   # degenerate/out-of-frame box — don't crash the run
            return pyfeat.detect(crop)

        return _classify

    raise ValueError(f"Unknown expression backend: {backend!r}")

# ── Session mode ─────────────────────────────────────────────────────────────

def _camera_index_from_filename(video_path: Path) -> int:
    """Parses the real camera index from a MOSAIC-recorded video's own
    filename (e.g. "video_2.mp4" -> 2), instead of trusting a video list's
    positional loop index — mirrors run_pose.py's identical helper/rationale
    (VideoManager preserves gaps in camera indices when an earlier camera
    fails to open during recording, so a plain enumerate() over a sorted
    file list can silently assign the wrong index). Falls back to 0 with a
    stderr warning if the filename doesn't match "video_N.<ext>"."""
    m = re.search(r"video_(\d+)", video_path.stem)
    if not m:
        print(f"[run_expression] Warning: could not parse a camera index from "
              f"{video_path.name}, defaulting to 0", file=sys.stderr)
        return 0
    return int(m.group(1))


def process_session(session_dir: Path, backend: str, max_faces: int,
                     min_confidence: float, skip: int) -> None:
    videos = sorted((session_dir / "video").glob("*.mp4"))
    if not videos:
        print(f"[run_expression] No .mp4 files found in {session_dir / 'video'}", file=sys.stderr)
        sys.exit(1)

    print(f"[run_expression] Found {len(videos)} video(s) in {session_dir / 'video'}", flush=True)

    # Own subfolder, not a sidecar beside the video — mirrors run_pose.py's
    # pose_dir / run_face_mask.py's anonymized/ folder convention, keeping
    # video/ from being cluttered with per-camera analysis sidecars.
    # Forward-only: sessions analyzed before this change have their
    # .expression.json under video/ instead and won't be found here until
    # re-run (this project's established no-migration convention, item 13).
    expression_dir = session_dir / "expression"

    # Detector + classifier are built once and reused across every video in
    # the session — a fresh MediaPipeExpressionDetector/FerPlusClassifier
    # per file would reload model weights once per camera instead of once
    # per run (the same redundant-reload mistake item 17's diarization
    # plugin had to fix after code review).
    detector = MediaPipeExpressionDetector(max_faces=max_faces, min_confidence=min_confidence)
    classify_fn = _make_classify_fn(backend)

    # "Camera N/M:" banner — distinct, easily-regexed format matching
    # run_pose.py's/run_face_mask.py's own convention — drives AnalysisTabW's
    # coarser, session-wide progress bar (parsed alongside the finer
    # per-frame one). Uses `position` (this video's place in the loop), not
    # camera_index (which can have gaps).
    for position, video_path in enumerate(videos, start=1):
        camera_index = _camera_index_from_filename(video_path)
        print(f"[run_expression] Camera {position}/{len(videos)}: {video_path.name}", flush=True)
        process_video(video_path, detector, classify_fn, backend, skip=skip,
                      camera_index=camera_index, output_dir=expression_dir)


def process_video(video_path: Path, detector: MediaPipeExpressionDetector,
                   classify_fn: ClassifyFn, backend: str, skip: int = 1,
                   camera_index: int = 0, output_dir: Optional[Path] = None) -> None:
    skip = max(1, skip)   # --skip 0 would divide-by-zero on frame_idx % skip below
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        print(f"[run_expression] Cannot open {video_path}", file=sys.stderr)
        return

    fps   = cap.get(cv2.CAP_PROP_FPS) or 30.0
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    print(f"[run_expression] Processing {video_path.name}  ({total} frames @ {fps:.1f} fps, "
          f"backend={backend})", flush=True)

    # Scales to the video's own length instead of a fixed 100-frame stride
    # (matches run_pose.py's/run_face_mask.py's identical fix): short
    # session videos get a progress update every frame, long videos are
    # capped at ~200 total prints.
    progress_interval = max(1, total // 200)

    # Built directly from camera_index (not a naive "video" -> "timestamps_cam"
    # substring-replace on the video's own stem) — that substitution turns
    # "video_0" into "timestamps_cam_0.csv" (stray underscore), which never
    # matches the real "timestamps_cam0.csv" file, silently leaving every
    # frame's timestamp at 0 and collapsing the whole Analysis-tab chart onto
    # a single point at ms=0. Same bug run_pose.py already found and fixed;
    # see this session's plan notes for the other 2 sibling scripts it was
    # also found and fixed in.
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

        # Frames are NOT emitted on skipped indices — gaps are handled by
        # ExpressionResult::nearest_frame() on the C++ side, exactly like
        # pose (this plugin never rewrites video, so face-mask's "reuse the
        # last detection on skipped frames" concern doesn't apply here).
        if frame_idx % skip == 0:
            ts_ns = timestamps[frame_idx] if frame_idx < len(timestamps) else 0
            faces = detector.detect(frame)
            results.append(_frame_to_dict(frame_idx, ts_ns, camera_index, faces, frame, classify_fn))

        frame_idx += 1
        if frame_idx % progress_interval == 0:
            elapsed = time.perf_counter() - t_start
            pct = frame_idx / max(total, 1) * 100
            print(f"  {pct:5.1f}%  ({frame_idx}/{total})  {elapsed:.1f}s elapsed", flush=True)

    cap.release()
    elapsed = time.perf_counter() - t_start
    print(f"[run_expression] Done. {frame_idx} frames in {elapsed:.1f}s "
          f"({frame_idx / max(elapsed, 1e-9):.1f} fps throughput)", flush=True)

    _write_results(video_path, results, backend, output_dir)


def _frame_to_dict(frame_idx: int, ts_ns: int, camera_index: int,
                    faces: list[FaceExpression], frame_bgr: np.ndarray,
                    classify_fn: ClassifyFn) -> dict:
    subjects = []
    # subject_id is plain per-frame enumerate order — no cross-frame
    # identity tracking, same caveat as PoseSubject.subjectId.
    for subject_id, face in enumerate(faces):
        dominant, score, aus = classify_fn(face, frame_bgr)
        subject = {
            "subject_id": subject_id,
            "confidence": round(face.confidence, 3),
            "bbox_xyxy": [round(v, 1) for v in face.bbox_xyxy],
            "blendshape_scores": [round(v, 4) for v in face.blendshape_scores],
            "dominant_expression": dominant,
            "dominant_score": round(score, 3),
        }
        # Only the pyfeat backend produces AU data — the key is entirely
        # absent (not null) for the other 2 backends, keeping their JSON
        # output byte-for-byte unchanged.
        if aus is not None:
            subject["action_units"] = aus
        subjects.append(subject)
    return {"frame_index": frame_idx, "timestamp_ns": ts_ns,
            "camera_index": camera_index, "subjects": subjects}


def _write_results(video_path: Path, results: list[dict], backend: str,
                    output_dir: Optional[Path] = None) -> None:
    if output_dir is not None:
        output_dir.mkdir(parents=True, exist_ok=True)
        out_path = output_dir / f"{video_path.stem}.expression.json"
    else:
        # Standalone --video CLI mode has no session/expression-folder
        # concept to hang a subfolder off — keep today's beside-the-video
        # behavior unchanged, matching run_pose.py's identical fallback.
        out_path = video_path.with_suffix("").with_suffix(".expression.json")
    doc = {
        "source_video": video_path.name,
        "backend": backend,
        "blendshape_names": BLENDSHAPE_NAMES,
        "frames": results,
    }
    if backend == "pyfeat":
        from expression.pyfeat import AU_NAMES
        doc["au_names"] = AU_NAMES
    out_path.write_text(json.dumps(doc, indent=2))
    print(f"[run_expression] Expression data → {out_path}", flush=True)

# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    args = parse_args()

    if args.session:
        process_session(Path(args.session), args.backend, args.max_faces,
                         args.min_confidence, args.skip)
    elif args.video:
        detector = MediaPipeExpressionDetector(
            max_faces=args.max_faces, min_confidence=args.min_confidence)
        classify_fn = _make_classify_fn(args.backend)
        video_path = Path(args.video)
        process_video(video_path, detector, classify_fn, args.backend, skip=args.skip,
                      camera_index=_camera_index_from_filename(video_path))


if __name__ == "__main__":
    main()
