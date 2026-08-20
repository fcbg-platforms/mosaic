"""
MOSAIC multi-camera 3D gaze fusion runner — for each calibrated camera in a
recorded session, detects a 3D gaze ray per analysed frame
(analysis/gaze/estimator.py), then fuses whichever cameras' rays are
contemporaneous (per the session's sync_manifest.json master-tick
alignment — the same post-hoc timing mechanism SessionPlayerW already uses
for synchronised playback, reused here rather than inventing a second sync
mechanism) into one triangulated 3D gaze origin/direction per tick, plus
(when the room's reference plane is defined) the point where that ray
intersects it (analysis/gaze/ray_math.py). Writes a single session-root
sidecar "gaze_fusion.json" — unlike Pose/Face-Mask/Expression's per-video
sidecars, this plugin's output is inherently cross-camera.

Only cameras with BOTH intrinsic (`calibrated`) and extrinsic
(`extrinsic_calibrated`) calibration in session_meta.json contribute — a
camera missing either can't produce a metric, room-space ray, so it's
skipped rather than silently distorting the fusion with a bogus one.

    python analysis/run_gaze_fusion.py --session /path/to/session_2026-06-07_14-30

See analysis/README.rst for full documentation.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
from gaze.estimator import EYE_ORIGIN_MODEL_MM, FaceGazeSample, MediaPipeGazeEstimator3D
from gaze.ray_math import (
    camera_ray_from_pose,
    closest_point_of_rays,
    ray_plane_intersection,
    transform_ray_to_room,
)

# ── Data ──────────────────────────────────────────────────────────────────


@dataclass
class _CameraInfo:
    index: int
    video_file: str
    camera_matrix: np.ndarray  # 3x3
    dist_coeffs: np.ndarray  # length-5
    extrinsic_rt: np.ndarray  # length-16 flat, row-major 4x4


@dataclass
class _AnalysedFrame:
    # The camera's own hardware frame counter (matches sync_manifest.json's
    # ticks["camN_frame_ids"] values and timestamps_camN.csv's "frame_id"
    # column) — deliberately NOT the plain 0-based video-decode position.
    # Those two only coincide if this camera never dropped an earlier frame
    # in the recording; VideoGrabber assigns frame_id BEFORE the ring-buffer
    # drop check, so any drop permanently offsets frame_id ahead of decode
    # position from that point on. Reading the real value from the CSV
    # (instead of assuming frame_id == decode_index + 1) keeps cross-camera
    # matching correct even on a session with dropped frames.
    frame_id: int
    timestamp_ns: int
    sample: FaceGazeSample | None  # None if no face detected on this frame


# ── CLI ───────────────────────────────────────────────────────────────────


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MOSAIC multi-camera 3D gaze fusion")
    parser.add_argument(
        "--session", required=True, metavar="DIR", help="Recorded session directory"
    )
    parser.add_argument(
        "--min-cameras",
        type=int,
        default=2,
        help="Minimum simultaneous cameras required to compute a target point "
        "(default: 2). Below this, per-camera rays are still recorded.",
    )
    parser.add_argument(
        "--min-confidence",
        type=float,
        default=0.5,
        help="Face detection/presence confidence threshold (default: 0.5)",
    )
    parser.add_argument(
        "--skip",
        type=int,
        default=1,
        help="Process every Nth frame per camera (default: 1 = every frame)",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    session_dir = Path(args.session)

    meta = _load_json(session_dir / "session_meta.json")
    if meta is None:
        print(f"[run_gaze_fusion] Cannot read session_meta.json in {session_dir}", file=sys.stderr)
        sys.exit(1)

    manifest = _load_json(session_dir / "sync_manifest.json")
    if manifest is None:
        print(
            f"[run_gaze_fusion] No sync_manifest.json in {session_dir} — "
            "the caller (AnalysisManager) generates one before launching this script; "
            "if running standalone, generate it via the app first.",
            file=sys.stderr,
        )
        sys.exit(1)

    cameras = _resolve_cameras(meta, manifest)
    if not cameras:
        print(
            "[run_gaze_fusion] No camera has both intrinsic AND extrinsic calibration — "
            "nothing to fuse. Run Room (Extrinsics) calibration first.",
            file=sys.stderr,
        )
        sys.exit(1)

    skip = max(1, args.skip)
    detector = MediaPipeGazeEstimator3D(max_faces=1, min_confidence=args.min_confidence)

    frames_by_camera: dict[int, list[_AnalysedFrame]] = {}
    for cam in cameras:
        video_path = session_dir / cam.video_file
        if not video_path.exists():
            print(
                f"[run_gaze_fusion] Missing video for camera {cam.index}: {video_path}",
                file=sys.stderr,
            )
            continue
        frames_by_camera[cam.index] = _analyse_video(video_path, cam, detector, skip)

    room = meta.get("room", {}) or {}
    plane_defined = bool(room.get("plane_defined", False))
    plane_point = np.array(room.get("plane_point", [0.0, 0.0, 0.0]), dtype=np.float64)
    plane_normal = np.array(room.get("plane_normal", [0.0, 0.0, 1.0]), dtype=np.float64)

    frame_results = _fuse_ticks(
        manifest,
        cameras,
        frames_by_camera,
        args.min_cameras,
        plane_defined,
        plane_point,
        plane_normal,
    )

    _write_results(
        session_dir, cameras, manifest, plane_defined, plane_point, plane_normal, frame_results
    )


# ── Session metadata ────────────────────────────────────────────────────────


def _load_json(path: Path) -> dict | None:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text())
    except (json.JSONDecodeError, OSError):
        return None


def _resolve_cameras(meta: dict, manifest: dict) -> list[_CameraInfo]:
    calib_by_index: dict[int, dict] = {}
    for cam in meta.get("cameras", []):
        calib_by_index[cam.get("index", -1)] = cam.get("calibration", {}) or {}

    cameras: list[_CameraInfo] = []
    for cam in manifest.get("cameras", []):
        idx = cam.get("index", -1)
        calib = calib_by_index.get(idx)
        if not calib or not calib.get("calibrated") or not calib.get("extrinsic_calibrated"):
            continue  # needs BOTH intrinsics and extrinsics for a metric, room-space ray

        cm, dc, ex = calib.get("camera_matrix"), calib.get("dist_coeffs"), calib.get("extrinsic_rt")
        if not cm or not dc or not ex:
            continue

        cameras.append(
            _CameraInfo(
                index=idx,
                video_file=cam.get("video_file", f"video/video_{idx}.mp4"),
                camera_matrix=np.array(cm, dtype=np.float64).reshape(3, 3),
                dist_coeffs=np.array(dc, dtype=np.float64),
                extrinsic_rt=np.array(ex, dtype=np.float64),
            )
        )
    return cameras


# ── Per-camera analysis ──────────────────────────────────────────────────────


def _analyse_video(
    video_path: Path, cam: _CameraInfo, detector: MediaPipeGazeEstimator3D, skip: int
) -> list[_AnalysedFrame]:
    # Same timestamp-CSV lookup convention as run_pose.py/run_expression.py,
    # extended to also read "frame_id" (see _AnalysedFrame.frame_id's doc
    # comment for why this must come from the CSV, not be assumed). Built
    # directly from cam.index rather than a naive "video" -> "timestamps_cam"
    # substring-replace on the video's own stem — that substitution turns
    # "video_0" into "timestamps_cam_0.csv" (stray underscore), which never
    # matches the real "timestamps_cam0.csv" file (same bug run_pose.py/
    # run_expression.py already found and fixed).
    ts_csv = video_path.with_name(f"timestamps_cam{cam.index}.csv")
    timestamps: list[int] = []
    frame_ids: list[int] = []
    if ts_csv.exists():
        with ts_csv.open() as f:
            for row in csv.DictReader(f):
                try:
                    timestamps.append(int(row.get("elapsed_ns", 0)))
                except ValueError:
                    timestamps.append(0)
                try:
                    frame_ids.append(int(row.get("frame_id", 0)))
                except ValueError:
                    frame_ids.append(0)

    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        print(f"[run_gaze_fusion] Cannot open {video_path}", file=sys.stderr)
        return []

    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    print(
        f"[run_gaze_fusion] Analysing camera {cam.index}: {video_path.name} " f"({total} frames)",
        flush=True,
    )

    results: list[_AnalysedFrame] = []
    frame_idx = 0
    t_start = time.perf_counter()
    progress_interval = max(1, total // 200)

    while True:
        ok, frame = cap.read()
        if not ok:
            break

        if frame_idx % skip == 0:
            ts_ns = timestamps[frame_idx] if frame_idx < len(timestamps) else 0
            # Fall back to frame_idx+1 only if the CSV is missing/short —
            # degrading gracefully, same spirit as the elapsed_ns fallback above.
            frame_id = frame_ids[frame_idx] if frame_idx < len(frame_ids) else frame_idx + 1
            samples = detector.detect(frame, cam.camera_matrix, cam.dist_coeffs)
            sample = samples[0] if samples else None  # subject 0 only — no cross-frame identity
            results.append(_AnalysedFrame(frame_id=frame_id, timestamp_ns=ts_ns, sample=sample))

        frame_idx += 1
        if frame_idx % progress_interval == 0:
            elapsed = time.perf_counter() - t_start
            pct = frame_idx / max(total, 1) * 100
            print(
                f"  cam{cam.index}  {pct:5.1f}%  ({frame_idx}/{total})  {elapsed:.1f}s elapsed",
                flush=True,
            )

    cap.release()
    n_faces = sum(1 for r in results if r.sample is not None)
    print(
        f"[run_gaze_fusion] cam{cam.index} done: {frame_idx} frames analysed, "
        f"{n_faces} with a detected face",
        flush=True,
    )
    return results


def _nearest_analysed_frame(
    sorted_ids: list[int], frames: list[_AnalysedFrame], frame_id_target: int
) -> _AnalysedFrame | None:
    """Binary-search nearest-frame lookup BY frame_id (the shared hardware
    frame counter — see _AnalysedFrame.frame_id), mirroring
    ExpressionResult::nearest_frame()'s tie-break (ties go to the earlier
    frame) — needed because --skip means not every sync-manifest frame_id
    was actually analysed. sorted_ids must be frames' frame_id values in the
    same order as frames — precomputed ONCE per camera by the caller, not
    rebuilt on every call (this runs once per (tick, camera) pair)."""
    if not frames:
        return None
    pos = bisect.bisect_left(sorted_ids, frame_id_target)
    if pos == 0:
        return frames[0]
    if pos == len(frames):
        return frames[-1]
    before, after = frames[pos - 1], frames[pos]
    before_delta = frame_id_target - before.frame_id
    after_delta = after.frame_id - frame_id_target
    return before if before_delta <= after_delta else after


# ── Fusion ────────────────────────────────────────────────────────────────


def _fuse_ticks(
    manifest: dict,
    cameras: list[_CameraInfo],
    frames_by_camera: dict[int, list[_AnalysedFrame]],
    min_cameras: int,
    plane_defined: bool,
    plane_point: np.ndarray,
    plane_normal: np.ndarray,
) -> list[dict]:
    total_ticks = int(manifest.get("total_ticks", 0))
    ticks_obj = manifest.get("ticks", {}) or {}
    master_fps = float(manifest.get("master_fps", 25.0))
    t_origin_ns = int(manifest.get("t_origin_ns", "0"))
    step_ns = int(manifest.get("step_ns", str(int(1e9 / master_fps))))

    # Precomputed ONCE per camera (not per tick — this loop runs once per
    # (tick, camera) pair, so re-deriving either of these inside it would be
    # an O(total_ticks) rebuild on every single lookup): each camera's
    # analysed-frame frame_id list (for the nearest_frame bisect) and its
    # sync-manifest tick->frame_id array (avoids an f-string dict lookup
    # per tick too).
    sorted_ids_by_camera: dict[int, list[int]] = {
        cam.index: [f.frame_id for f in frames_by_camera[cam.index]]
        for cam in cameras
        if frames_by_camera.get(cam.index)
    }
    tick_ids_by_camera: dict[int, list[int]] = {
        cam.index: ticks_obj.get(f"cam{cam.index}_frame_ids", [])
        for cam in cameras
        if frames_by_camera.get(cam.index)
    }

    results: list[dict] = []
    for tick in range(total_ticks):
        tick_timestamp_ns = t_origin_ns + tick * step_ns

        contributions = []
        for cam in cameras:
            frames = frames_by_camera.get(cam.index)
            if not frames:
                continue

            ids = tick_ids_by_camera[cam.index]
            if tick >= len(ids):
                continue
            frame_id = ids[tick]
            if frame_id < 0:
                continue  # SyncManifest's own "-1 = no frame for this camera at this tick"

            analysed = _nearest_analysed_frame(sorted_ids_by_camera[cam.index], frames, frame_id)
            if analysed is None or analysed.sample is None:
                continue

            sample = analysed.sample
            origin_cam, direction_cam = camera_ray_from_pose(
                sample.head_rotation,
                sample.head_translation,
                sample.gaze_dx,
                sample.gaze_dy,
                EYE_ORIGIN_MODEL_MM,
            )
            origin_room, direction_room = transform_ray_to_room(
                origin_cam, direction_cam, cam.extrinsic_rt
            )

            contributions.append(
                {
                    "camera_index": cam.index,
                    "face_box_px": [round(v, 1) for v in sample.bbox_xyxy],
                    "gaze_dx": round(sample.gaze_dx, 4),
                    "gaze_dy": round(sample.gaze_dy, 4),
                    "origin_room": origin_room,
                    "direction_room": direction_room,
                    "confidence": sample.confidence,
                }
            )

        if not contributions:
            results.append(
                {
                    "tick": tick,
                    "timestamp_ns": tick_timestamp_ns,
                    "num_cameras": 0,
                    "is_triangulated": False,
                    "per_camera": [],
                }
            )
            continue

        origins = [c["origin_room"] for c in contributions]
        directions = [c["direction_room"] for c in contributions]
        fused_origin, residual_rms = closest_point_of_rays(origins, directions)

        # A single "fused direction" for display: mean of contributing
        # directions, renormalised. residual_rms (not this) is what
        # actually communicates fit quality/spread.
        fused_direction = np.mean(directions, axis=0)
        norm = np.linalg.norm(fused_direction)
        if norm > 1e-9:
            fused_direction = fused_direction / norm

        is_triangulated = len(contributions) >= 2
        target = None
        if plane_defined and len(contributions) >= min_cameras:
            target = ray_plane_intersection(
                fused_origin, fused_direction, plane_point, plane_normal
            )

        frame_entry = {
            "tick": tick,
            "timestamp_ns": tick_timestamp_ns,
            "num_cameras": len(contributions),
            "is_triangulated": is_triangulated,
            "fused_origin_room": fused_origin.tolist(),
            "fused_direction_room": fused_direction.tolist(),
            "residual_rms_mm": round(residual_rms, 2) if is_triangulated else None,
            "per_camera": [
                {
                    **c,
                    "origin_room": c["origin_room"].tolist(),
                    "direction_room": c["direction_room"].tolist(),
                }
                for c in contributions
            ],
        }
        if target is not None:
            frame_entry["target_point_room"] = target.tolist()

        results.append(frame_entry)

    return results


# ── Output ────────────────────────────────────────────────────────────────


def _write_results(
    session_dir: Path,
    cameras: list[_CameraInfo],
    manifest: dict,
    plane_defined: bool,
    plane_point: np.ndarray,
    plane_normal: np.ndarray,
    frame_results: list[dict],
) -> None:
    out_path = session_dir / "gaze_fusion.json"
    out_path.write_text(
        json.dumps(
            {
                "schema": "mosaic-gaze-fusion-v1",
                "source_videos": [cam.video_file for cam in cameras],
                # Static per-camera room position (extrinsic_rt's translation
                # column) — for the room-view widget's camera icons. Written once
                # here rather than repeated per frame, since it never changes
                # within one fusion run.
                "cameras": [
                    {
                        "index": cam.index,
                        "position_room": [
                            float(cam.extrinsic_rt[3]),
                            float(cam.extrinsic_rt[7]),
                            float(cam.extrinsic_rt[11]),
                        ],
                    }
                    for cam in cameras
                ],
                "plane": {
                    "defined": plane_defined,
                    "point": plane_point.tolist(),
                    "normal": plane_normal.tolist(),
                },
                "master_fps": manifest.get("master_fps", 25.0),
                "frames": frame_results,
            },
            indent=2,
        )
    )
    print(f"[run_gaze_fusion] Gaze fusion data → {out_path}", flush=True)


if __name__ == "__main__":
    main()
