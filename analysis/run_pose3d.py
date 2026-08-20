"""
MOSAIC multi-camera 3D pose reconstruction runner — triangulates each
camera's already-computed 2D COCO keypoints (analysis/run_pose.py's
".pose.json" sidecars) into 3D room-space skeletons, using the room
extrinsic calibration from item 19's ChArUco room calibration
(session_meta.json's per-camera "calibration" block) and the session's
sync_manifest.json master-tick alignment — the same post-hoc timing
mechanism analysis/run_gaze_fusion.py already uses, reused here rather than
inventing a second sync mechanism.

Unlike gaze fusion (which hard-codes "subject 0 only"), this plugin
attempts real cross-camera person association (analysis/pose3d/association.py)
so multi-person sessions produce multiple, correctly-separated 3D people,
each carrying a persistent track_id across frames
(analysis/pose3d/tracker.py). Per-keypoint triangulation itself
(analysis/pose3d/triangulation.py) is a classic multi-view linear DLT with
real reprojection-error-based outlier-view rejection.

PREREQUISITE: this script reads .pose.json sidecars, it does not run any
pose inference itself — the Pose plugin (analysis/run_pose.py) must already
have been run on the session for at least 2 cameras. Writes a single
session-root sidecar "skeleton3d.json" — unlike Pose/Face-Mask/Expression's
per-video sidecars, this plugin's output is inherently cross-camera, like
gaze_fusion.json.

    python analysis/run_pose3d.py --session /path/to/session_2026-06-07_14-30

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

import numpy as np
from pose3d.association import PersonObservation, cluster_people
from pose3d.smoothing import smooth_track_positions
from pose3d.tracker import PersonTracker3D
from pose3d.triangulation import CameraGeom, project_point_px, triangulate_with_rejection

# ── Data ──────────────────────────────────────────────────────────────────


@dataclass
class _AnalysedFrame:
    # The camera's own hardware frame counter — see run_gaze_fusion.py's
    # identical _AnalysedFrame.frame_id doc comment for why this must come
    # from timestamps_camN.csv's "frame_id" column rather than being
    # assumed to equal the .pose.json frame's 0-based list position.
    frame_id: int
    timestamp_ns: int
    subjects: list  # list of {"keypoints_px": (K,2) ndarray, "visibilities": (K,) ndarray}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MOSAIC multi-camera 3D pose reconstruction")
    parser.add_argument(
        "--session", required=True, metavar="DIR", help="Recorded session directory"
    )
    parser.add_argument(
        "--min-cameras",
        type=int,
        default=2,
        help="Minimum cameras a person cluster must span to be "
        "reconstructed (default: 2 — the mathematical minimum "
        "for triangulation)",
    )
    parser.add_argument(
        "--max-reprojection-error-px",
        type=float,
        default=15.0,
        help="Per-view reprojection error threshold (px) for outlier-view "
        "rejection during keypoint triangulation (default: 15.0)",
    )
    parser.add_argument(
        "--max-pair-cost-px",
        type=float,
        default=20.0,
        help="Cross-camera person-association cost threshold (px) — "
        "pairs above this are treated as different people (default: 20.0)",
    )
    parser.add_argument(
        "--min-shared-keypoints",
        type=int,
        default=4,
        help="Minimum mutually-visible keypoints required to assess "
        "whether two detections are the same person (default: 4)",
    )
    parser.add_argument(
        "--max-track-gap-ticks",
        type=int,
        default=5,
        help="Ticks a track may go unmatched before it's considered ended " "(default: 5)",
    )
    parser.add_argument(
        "--max-track-jump-mm",
        type=float,
        default=400.0,
        help="Maximum centroid movement (mm) between ticks for the same " "track (default: 400.0)",
    )
    parser.add_argument(
        "--skip",
        type=int,
        default=1,
        help="Process every Nth master tick (default: 1 = every tick)",
    )
    parser.add_argument(
        "--smoothing-window",
        type=int,
        default=1,
        help="Centered per-track median filter width (in valid ticks, not "
        "raw tick count) applied to each track's 'keypoints_room_smoothed' "
        "output field (default: 1 = off). The raw 'keypoints_room' field is "
        "always written unchanged regardless of this setting.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    session_dir = Path(args.session)

    meta = _load_json(session_dir / "session_meta.json")
    if meta is None:
        print(f"[run_pose3d] Cannot read session_meta.json in {session_dir}", file=sys.stderr)
        sys.exit(1)

    manifest = _load_json(session_dir / "sync_manifest.json")
    if manifest is None:
        print(
            f"[run_pose3d] No sync_manifest.json in {session_dir} — "
            "the caller (AnalysisManager) generates one before launching this script; "
            "if running standalone, generate it via the app first.",
            file=sys.stderr,
        )
        sys.exit(1)

    cameras = _resolve_cameras(meta, manifest)
    if not cameras:
        print(
            "[run_pose3d] No camera has both intrinsic AND extrinsic calibration — "
            "nothing to reconstruct. Run Room (Extrinsics) calibration first.",
            file=sys.stderr,
        )
        sys.exit(1)

    camera_geoms = {
        idx: CameraGeom(
            index=idx,
            camera_matrix=info["camera_matrix"],
            dist_coeffs=info["dist_coeffs"],
            extrinsic_rt=info["extrinsic_rt"],
        )
        for idx, info in cameras.items()
    }

    frames_by_camera: dict[int, list[_AnalysedFrame]] = {}
    for idx, info in cameras.items():
        # Pose's own sidecar output moved to a dedicated session_dir/pose/
        # subfolder (see AnalysisTabW::pose_json_path_for()) and is now
        # namespaced by which model produced it (e.g.
        # "video_0.yolov8n-pose.pose.json") so running a second model
        # against the same session no longer clobbers the first — 3D
        # reconstruction doesn't care which model was used, just that some
        # pose data exists per camera, so glob for any match and take the
        # most recently written one rather than assuming a fixed bare
        # filename (which the C++ side no longer ever writes).
        stem = Path(info["video_file"]).stem
        candidates = sorted(
            (session_dir / "pose").glob(f"{stem}.*.pose.json"), key=lambda p: p.stat().st_mtime
        )
        if not candidates:
            continue
        pose_json = candidates[-1]
        frames_by_camera[idx] = _load_pose_json(pose_json, session_dir / info["video_file"], idx)

    n_with_pose = sum(1 for v in frames_by_camera.values() if v)
    if n_with_pose == 0:
        print(
            "[run_pose3d] No .pose.json found for any calibrated camera — "
            "run the Pose plugin first.",
            file=sys.stderr,
        )
        sys.exit(1)
    if n_with_pose < args.min_cameras:
        print(
            f"[run_pose3d] Only {n_with_pose} camera(s) have .pose.json output "
            f"(need >= {args.min_cameras}) — run the Pose plugin on more cameras.",
            file=sys.stderr,
        )
        sys.exit(1)

    frame_results, keypoint_count = _reconstruct_ticks(
        manifest, camera_geoms, frames_by_camera, args
    )

    _apply_temporal_smoothing(frame_results, keypoint_count, args.smoothing_window)

    _write_results(session_dir, cameras, manifest, keypoint_count, args, frame_results)


# ── Session metadata ────────────────────────────────────────────────────────


def _load_json(path: Path) -> dict | None:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text())
    except (json.JSONDecodeError, OSError):
        return None


def _resolve_cameras(meta: dict, manifest: dict) -> dict:
    """Mirrors run_gaze_fusion.py's _resolve_cameras() — needs BOTH
    intrinsic (calibrated) and extrinsic (extrinsic_calibrated) calibration
    for a metric, room-space reconstruction. Returns
    {camera_index: {"video_file", "camera_matrix", "dist_coeffs", "extrinsic_rt"}}."""
    calib_by_index: dict = {}
    for cam in meta.get("cameras", []):
        calib_by_index[cam.get("index", -1)] = cam.get("calibration", {}) or {}

    cameras: dict = {}
    for cam in manifest.get("cameras", []):
        idx = cam.get("index", -1)
        calib = calib_by_index.get(idx)
        if not calib or not calib.get("calibrated") or not calib.get("extrinsic_calibrated"):
            continue

        cm, dc, ex = calib.get("camera_matrix"), calib.get("dist_coeffs"), calib.get("extrinsic_rt")
        if not cm or not dc or not ex:
            continue

        cameras[idx] = {
            "video_file": cam.get("video_file", f"video/video_{idx}.mp4"),
            "camera_matrix": np.array(cm, dtype=np.float64).reshape(3, 3),
            "dist_coeffs": np.array(dc, dtype=np.float64),
            "extrinsic_rt": np.array(ex, dtype=np.float64).reshape(4, 4),
        }
    return cameras


# ── Per-camera .pose.json loading ────────────────────────────────────────────


def _load_pose_json(
    pose_json_path: Path, video_path: Path, camera_index: int
) -> list[_AnalysedFrame]:
    """Loads a .pose.json sidecar (written by analysis/run_pose.py) and
    keys each frame by its real hardware frame_id, read from
    timestamps_camN.csv — NOT the .pose.json's own "frame_index" (which,
    per run_pose.py's frame-skip convention, is the analysed frame's
    position among analysed frames, not the source video's decode index).
    camera_index comes from the caller (the file's PATH / sync_manifest.json
    entry), not the .pose.json's own "camera_index" field — run_pose.py now
    writes that field correctly, but this script has never depended on it."""
    data = _load_json(pose_json_path)
    if not data:
        return []

    # Built directly from camera_index rather than a naive "video" ->
    # "timestamps_cam" substring-replace on the video's own stem — that
    # substitution turns "video_0" into "timestamps_cam_0.csv" (stray
    # underscore), which never matches the real "timestamps_cam0.csv" file,
    # silently falling back to frame_index+1 for frame_id and reintroducing
    # exactly the dropped-frame misalignment bug item 19's own review caught
    # and fixed once already for gaze fusion (same bug run_pose.py/
    # run_expression.py/run_gaze_fusion.py already found and fixed).
    ts_csv = video_path.with_name(f"timestamps_cam{camera_index}.csv")
    frame_ids_by_index: dict = {}
    if ts_csv.exists():
        with ts_csv.open() as f:
            for row_idx, row in enumerate(csv.DictReader(f)):
                try:
                    frame_ids_by_index[row_idx] = int(row.get("frame_id", row_idx + 1))
                except ValueError:
                    frame_ids_by_index[row_idx] = row_idx + 1

    results: list[_AnalysedFrame] = []
    for frame_obj in data.get("frames", []):
        decode_index = int(frame_obj.get("frame_index", 0))
        frame_id = frame_ids_by_index.get(decode_index, decode_index + 1)
        timestamp_ns = int(frame_obj.get("timestamp_ns", 0))

        subjects = []
        for subj in frame_obj.get("subjects", []):
            kps = np.array(subj.get("keypoints", []), dtype=np.float64)
            vis = np.array(subj.get("visibilities", []), dtype=np.float64)
            if kps.ndim != 2 or kps.shape[0] == 0:
                continue
            subjects.append({"keypoints_px": kps, "visibilities": vis})

        results.append(
            _AnalysedFrame(frame_id=frame_id, timestamp_ns=timestamp_ns, subjects=subjects)
        )

    results.sort(key=lambda f: f.frame_id)
    return results


def _nearest_analysed_frame(sorted_ids: list, frames: list, frame_id_target: int):
    """Binary-search nearest-frame lookup BY frame_id, mirroring
    run_gaze_fusion.py's _nearest_analysed_frame() exactly (tie goes to the
    earlier frame). sorted_ids must be frames' frame_id values in the same
    order as frames — precomputed ONCE per camera by the caller, not
    rebuilt on every call."""
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


# ── Reconstruction ───────────────────────────────────────────────────────────


def _reconstruct_ticks(
    manifest: dict, cameras: dict, frames_by_camera: dict, args: argparse.Namespace
):
    total_ticks = int(manifest.get("total_ticks", 0))
    ticks_obj = manifest.get("ticks", {}) or {}

    # Precomputed ONCE per camera — this loop runs once per (tick, camera)
    # pair, so re-deriving either of these inside it would be an
    # O(total_ticks) rebuild on every single lookup (the exact bug item
    # 19's own code review caught and fixed for run_gaze_fusion.py).
    sorted_ids_by_camera = {
        idx: [f.frame_id for f in frames] for idx, frames in frames_by_camera.items() if frames
    }
    tick_ids_by_camera = {
        idx: ticks_obj.get(f"cam{idx}_frame_ids", [])
        for idx in frames_by_camera
        if frames_by_camera.get(idx)
    }

    tracker = PersonTracker3D(
        max_gap_ticks=args.max_track_gap_ticks, max_jump_mm=args.max_track_jump_mm
    )

    keypoint_count = 17  # COCO fallback — see analysis/pose/keypoints.py::COCO_KEYPOINTS
    found = False
    for frames in frames_by_camera.values():
        for f in frames:
            if f.subjects:
                keypoint_count = f.subjects[0]["keypoints_px"].shape[0]
                found = True
                break
        if found:
            break

    results: list[dict] = []
    t_start = time.perf_counter()
    progress_interval = max(1, total_ticks // 200)

    for tick in range(0, total_ticks, max(1, args.skip)):
        # Gather this tick's per-camera detections.
        observations_by_camera: dict = {}
        for idx, frames in frames_by_camera.items():
            ids = tick_ids_by_camera.get(idx, [])
            if tick >= len(ids):
                continue
            frame_id = ids[tick]
            if frame_id < 0:
                continue  # SyncManifest's own "-1 = no frame for this camera at this tick"

            analysed = _nearest_analysed_frame(sorted_ids_by_camera[idx], frames, frame_id)
            if analysed is None or not analysed.subjects:
                continue

            observations_by_camera[idx] = [
                PersonObservation(
                    camera_index=idx,
                    person_index=p_idx,
                    keypoints_px=subj["keypoints_px"],
                    visibilities=subj["visibilities"],
                )
                for p_idx, subj in enumerate(analysed.subjects)
            ]

        people_entries = []
        cluster_centroids = []
        clusters = cluster_people(
            observations_by_camera, cameras, args.max_pair_cost_px, args.min_shared_keypoints
        )

        for cluster in clusters:
            # cluster: list of (camera_index, person_index). --min-cameras
            # lets the user require MORE than cluster_people()'s own
            # mathematical-minimum-of-2 before a person is reconstructed at
            # all (association and reconstruction are separate gates — a
            # cluster can span exactly 2 cameras and still be a real match,
            # just a less redundant one).
            if len({cam_idx for cam_idx, _ in cluster}) < args.min_cameras:
                continue

            obs_by_cam = {}
            for cam_idx, p_idx in cluster:
                obs_by_cam[cam_idx] = observations_by_camera[cam_idx][p_idx]

            keypoints_room = [None] * keypoint_count
            keypoints_valid = [False] * keypoint_count
            reprojection_error = [None] * keypoint_count
            reprojected_px: dict = {c: [None] * keypoint_count for c in cameras}

            n_valid_kps = 0
            for kp_i in range(keypoint_count):
                per_kp_obs = {}
                for cam_idx, obs in obs_by_cam.items():
                    if kp_i >= obs.keypoints_px.shape[0]:
                        continue
                    per_kp_obs[cam_idx] = (obs.keypoints_px[kp_i], obs.visibilities[kp_i])

                tri = triangulate_with_rejection(
                    per_kp_obs, cameras, args.max_reprojection_error_px
                )
                if tri is None:
                    continue

                keypoints_room[kp_i] = tri.point_room.tolist()
                keypoints_valid[kp_i] = True
                reprojection_error[kp_i] = round(
                    float(np.mean(list(tri.per_view_error_px.values()))), 2
                )
                n_valid_kps += 1

                for cam_idx in cameras:
                    proj = project_point_px(tri.point_room, cameras[cam_idx])
                    reprojected_px[cam_idx][kp_i] = [round(float(v), 1) for v in proj]

            if n_valid_kps == 0:
                continue

            zipped_kps = zip(keypoints_room, keypoints_valid, strict=False)
            valid_points = np.array([p for p, v in zipped_kps if v])
            centroid = valid_points.mean(axis=0)

            people_entries.append(
                {
                    "source_cameras": sorted(obs_by_cam.keys()),
                    "keypoints_room": keypoints_room,
                    "keypoints_valid": keypoints_valid,
                    "reprojection_error_px": reprojection_error,
                    "reprojected_px": reprojected_px,
                    "_centroid": centroid,
                }
            )
            cluster_centroids.append(centroid)

        track_ids = tracker.update(tick, cluster_centroids)
        for entry, tid in zip(people_entries, track_ids, strict=False):
            entry["track_id"] = tid
            entry["num_contributing_cameras"] = len(entry["source_cameras"])
            del entry["_centroid"]

        tick_timestamp_ns = _tick_timestamp_ns(manifest, tick)
        results.append(
            {
                "tick": tick,
                "timestamp_ns": tick_timestamp_ns,
                "people": people_entries,
            }
        )

        if tick % progress_interval == 0:
            elapsed = time.perf_counter() - t_start
            print(
                f"[run_pose3d]  tick {tick}/{total_ticks}  "
                f"({len(people_entries)} people)  {elapsed:.1f}s elapsed",
                flush=True,
            )

    return results, keypoint_count


def _tick_timestamp_ns(manifest: dict, tick: int) -> int:
    t_origin_ns = int(manifest.get("t_origin_ns", "0"))
    master_fps = float(manifest.get("master_fps", 25.0))
    step_ns = int(manifest.get("step_ns", str(int(1e9 / master_fps))))
    return t_origin_ns + tick * step_ns


# ── Temporal smoothing ───────────────────────────────────────────────────────


def _apply_temporal_smoothing(results: list[dict], keypoint_count: int, window: int) -> None:
    """Adds a "keypoints_room_smoothed" field to every person entry across
    `results`, mutated in place. Groups occurrences by track_id first (each
    track's own trajectory is smoothed independently — never bleeding
    across different people or across a track-id reassignment), then per
    keypoint index applies smooth_track_positions() over that track's
    per-tick positions (None at exactly the ticks "keypoints_valid" is
    False for that keypoint, mirroring "keypoints_room"'s own convention).

    Always runs, even at window<=1 — smooth_track_positions() treats that
    as a documented no-op, so "keypoints_room_smoothed" is always present
    (equal to the raw values when smoothing is off), matching this schema's
    "raw is always written, smoothed is additive" convention (item 16 pose
    kinematics, rPPG's bpm/smoothed_bpm) rather than omitting the field."""
    occurrences_by_track: dict[int, list[tuple[int, int]]] = {}
    for entry_idx, entry in enumerate(results):
        for person_idx, person in enumerate(entry["people"]):
            occurrences_by_track.setdefault(person["track_id"], []).append((entry_idx, person_idx))
            person["keypoints_room_smoothed"] = [None] * keypoint_count

    for occurrences in occurrences_by_track.values():
        for kp_i in range(keypoint_count):
            positions: list[np.ndarray | None] = []
            for entry_idx, person_idx in occurrences:
                person = results[entry_idx]["people"][person_idx]
                if person["keypoints_valid"][kp_i]:
                    positions.append(np.array(person["keypoints_room"][kp_i], dtype=float))
                else:
                    positions.append(None)

            smoothed = smooth_track_positions(positions, window)

            for (entry_idx, person_idx), value in zip(occurrences, smoothed, strict=False):
                person = results[entry_idx]["people"][person_idx]
                person["keypoints_room_smoothed"][kp_i] = (
                    None if value is None else [round(float(v), 2) for v in value]
                )


# ── Output ────────────────────────────────────────────────────────────────


def _write_results(
    session_dir: Path,
    cameras: dict,
    manifest: dict,
    keypoint_count: int,
    args: argparse.Namespace,
    frame_results: list,
) -> None:
    from pose.keypoints import COCO_KEYPOINTS, COCO_SKELETON

    keypoint_names = (
        COCO_KEYPOINTS
        if keypoint_count == len(COCO_KEYPOINTS)
        else [f"kp{i}" for i in range(keypoint_count)]
    )
    skeleton_edges = COCO_SKELETON if keypoint_count == len(COCO_KEYPOINTS) else []

    out_frames = []
    for entry in frame_results:
        people = []
        for p in entry["people"]:
            people.append(
                {
                    "track_id": p["track_id"],
                    "num_contributing_cameras": p["num_contributing_cameras"],
                    "source_cameras": p["source_cameras"],
                    "keypoints_room": p["keypoints_room"],
                    "keypoints_room_smoothed": p["keypoints_room_smoothed"],
                    "keypoints_valid": p["keypoints_valid"],
                    "reprojection_error_px": p["reprojection_error_px"],
                    "reprojected_px": {str(k): v for k, v in p["reprojected_px"].items()},
                }
            )
        out_frames.append(
            {
                "tick": entry["tick"],
                "timestamp_ns": entry["timestamp_ns"],
                "people": people,
            }
        )

    out_path = session_dir / "skeleton3d.json"
    out_path.write_text(
        json.dumps(
            {
                "schema": "mosaic-skeleton3d-v1",
                "source_videos": [info["video_file"] for info in cameras.values()],
                "cameras": [
                    {
                        "index": idx,
                        "position_room": [
                            float(info["extrinsic_rt"][0, 3]),
                            float(info["extrinsic_rt"][1, 3]),
                            float(info["extrinsic_rt"][2, 3]),
                        ],
                    }
                    for idx, info in cameras.items()
                ],
                "keypoint_names": keypoint_names,
                "skeleton_edges": [list(e) for e in skeleton_edges],
                "master_fps": manifest.get("master_fps", 25.0),
                "params": {
                    "min_cameras": args.min_cameras,
                    "max_reprojection_error_px": args.max_reprojection_error_px,
                    "max_pair_cost_px": args.max_pair_cost_px,
                    "min_shared_keypoints": args.min_shared_keypoints,
                    "max_track_gap_ticks": args.max_track_gap_ticks,
                    "max_track_jump_mm": args.max_track_jump_mm,
                    "smoothing_window": args.smoothing_window,
                },
                "frames": out_frames,
            },
            indent=2,
        )
    )
    print(f"[run_pose3d] 3D pose reconstruction → {out_path}", flush=True)


if __name__ == "__main__":
    main()
