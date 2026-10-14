#!/usr/bin/env python3
"""
MOSAIC motion analysis — mouse centroid tracker.

Usage
-----
Session mode (processes all videos in a recorded session):
    python run_motion.py --session /path/to/session_dir [options]

Single video mode:
    python run_motion.py --video /path/to/video.mp4 [options]

Options
-------
--n-animals N       Expected number of animals (0 = unlimited, default 0)
--min-area N        Minimum blob area in px² (default 400)
--max-area N        Maximum blob area in px² (default 7000)
--max-dist N        Max centroid jump per frame in px (default 80)
--mm-per-px F       Scale factor: mm per pixel (default 1.0)
--skip N            Process every N-th frame (default 1)
--out-format FMT    Output format: csv | json | hdf5 (default csv)
--no-heatmap        Skip heatmap generation
--no-trajectory     Skip trajectory plot generation
--preview           Show live OpenCV preview window
--timestamps CSV    Path to timestamps_camN.csv for ns timestamps
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path

import cv2
import numpy as np

# ── local imports ─────────────────────────────────────────────────────────────
_HERE = Path(__file__).parent
sys.path.insert(0, str(_HERE))

from motion.centroid_tracker import CentroidTracker, draw_tracks  # noqa: E402
from motion.heatmap import (  # noqa: E402
    generate_heatmap,
    generate_trajectory_plot,
    generate_velocity_histogram,
)

# ── Timestamp loader ──────────────────────────────────────────────────────────


def load_timestamps(csv_path: str | None) -> list[int] | None:
    """Per-frame nanosecond timestamps from a MOSAIC timestamps CSV.

    The file's columns are ``frame_id,elapsed_ns,wall_ns,hw_timestamp_ns``:
    the time is ``elapsed_ns`` (the first column is the frame number). A file
    without that header is read from its first column.
    """
    if not csv_path:
        return None
    p = Path(csv_path)
    if not p.exists():
        return None
    ts: list[int] = []
    with p.open(newline="") as f:
        rows = list(csv.reader(f))
    if not rows:
        return None
    header = [h.strip() for h in rows[0]]
    col = header.index("elapsed_ns") if "elapsed_ns" in header else 0
    for row in rows:
        if len(row) > col and row[col].strip().lstrip("-").isdigit():
            ts.append(int(row[col]))
    return ts if ts else None


def camera_index_from_filename(video_path: Path) -> int | None:
    """The camera a session video belongs to, from its name: "video_2.mp4"
    is camera index 2 (shown as Camera 3). ``None`` for any other name
    ("video_2_old.mp4", "clip.avi"), the same strict rule as the app's
    camera_index_from_video_file() (src/video/camera_label.hpp)."""
    m = re.fullmatch(r"video_(\d+)", video_path.stem)
    return int(m.group(1)) if m else None


# ── Core video processor ──────────────────────────────────────────────────────


def process_video(
    video_path: Path,
    tracker: CentroidTracker,
    timestamps: list[int] | None,
    skip: int,
    preview: bool,
    camera_index: int = 0,
) -> tuple[list[dict], dict[int, list[tuple[float, float]]], list[float], tuple[int, int]]:
    """
    Run the tracker on a single video file.

    Returns
    -------
    rows
        Per-frame list of output dicts.
    trajectories
        Per-animal position lists for heatmap.
    velocities
        All non-zero velocities for histogram.
    frame_size
        (width, height)
    """
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        print(f"[motion] Cannot open {video_path}", file=sys.stderr)
        return [], {}, [], (640, 480)

    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

    print(
        f"[motion] {video_path.name}  {width}×{height}  {fps:.1f} fps  {total} frames", flush=True
    )

    rows: list[dict] = []
    trajectories: dict[int, list[tuple[float, float]]] = {}
    velocities: list[float] = []
    frame_idx = 0

    tracker.reset()

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        # The frame's time from the timestamps file; past its end (or with
        # none), continued from its last time at the video's frame rate, so
        # the times never jump back. Never the time it is analysed.
        if timestamps and frame_idx < len(timestamps):
            ts_ns = timestamps[frame_idx]
        elif timestamps:
            ts_ns = timestamps[-1] + round((frame_idx - len(timestamps) + 1) * 1e9 / fps)
        else:
            ts_ns = round(frame_idx * 1e9 / fps)

        if frame_idx % skip == 0:
            tracks = tracker.update(frame, timestamp_ns=ts_ns, fps=fps)

            for t in tracks:
                # Only animals seen in this frame: a track kept alive while
                # lost would repeat its last position, and its last speed,
                # for up to max_lost frames.
                if t.lost_count > 0 or t.position == (0.0, 0.0):
                    continue
                cx, cy = t.position
                # Consecutive positions are `skip` frames apart.
                vel_mm = t.velocity_mm_per_s(tracker.mm_per_px, fps / skip)

                rows.append(
                    {
                        "frame": frame_idx,
                        "timestamp_ns": ts_ns,
                        # Animal ids restart in every video: an animal is
                        # (camera_index, animal_id).
                        "camera_index": camera_index,
                        "animal_id": t.id,
                        "cx_px": round(cx, 2),
                        "cy_px": round(cy, 2),
                        "cx_mm": round(cx * tracker.mm_per_px, 3),
                        "cy_mm": round(cy * tracker.mm_per_px, 3),
                        "velocity_mm_s": round(vel_mm, 3),
                        "area_px": round(t.mean_area, 1),
                    }
                )

                trajectories.setdefault(t.id, []).append((cx, cy))
                if vel_mm > 0:
                    velocities.append(vel_mm)

            if preview:
                annotated = draw_tracks(frame, tracks, mm_per_px=tracker.mm_per_px, fps=fps / skip)
                cv2.imshow(f"MOSAIC Motion — {video_path.name}", annotated)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break

        frame_idx += 1

    cap.release()
    if preview:
        cv2.destroyAllWindows()

    print(f"[motion]   → {len(rows)} detections, {len(trajectories)} animals", flush=True)
    return rows, trajectories, velocities, (width, height)


# ── Output writers ────────────────────────────────────────────────────────────


def write_csv(rows: list[dict], out_path: Path) -> None:
    if not rows:
        return
    fields = list(rows[0].keys())
    with out_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def write_json(rows: list[dict], out_path: Path) -> None:
    with out_path.open("w") as f:
        json.dump(rows, f, indent=2)


def write_hdf5(rows: list[dict], out_path: Path) -> None:
    import h5py

    if not rows:
        return
    keys = list(rows[0].keys())
    arrays = {k: np.array([r[k] for r in rows]) for k in keys}
    with h5py.File(str(out_path), "w") as hf:
        for k, v in arrays.items():
            hf.create_dataset(k, data=v, compression="gzip")


# ── Session mode ──────────────────────────────────────────────────────────────


def process_session(args: argparse.Namespace) -> None:
    session_dir = Path(args.session)
    if not session_dir.is_dir():
        print(f"[motion] Session directory not found: {session_dir}", file=sys.stderr)
        sys.exit(1)

    video_dir = session_dir / "video"
    video_files = sorted(
        list(video_dir.glob("*.mp4"))
        + list(video_dir.glob("*.mkv"))
        + list(video_dir.glob("*.avi"))
    )
    if not video_files:
        print(f"[motion] No video files found in {video_dir}.", file=sys.stderr)
        sys.exit(1)

    tracker = CentroidTracker(
        min_area=args.min_area,
        max_area=args.max_area,
        max_distance=args.max_dist,
        mm_per_px=args.mm_per_px,
        n_animals=args.n_animals,
    )

    all_rows: list[dict] = []
    # Per camera: each video is its own view and its own set of animal ids,
    # so their positions never share one picture.
    per_camera: dict[int, tuple] = {}

    for vid in video_files:
        cam = camera_index_from_filename(vid)
        if cam is None:
            # Not a camera recording (a copy, an export): it would be read as
            # camera 0 and mixed with that camera's animals.
            print(f"[motion] Skipping {vid.name}: not named video_N", flush=True)
            continue
        ts_csv = video_dir / f"timestamps_cam{cam}.csv"
        timestamps = load_timestamps(str(ts_csv) if ts_csv.exists() else None)

        rows, trajectories, velocities, frame_size = process_video(
            vid, tracker, timestamps, skip=args.skip, preview=args.preview, camera_index=cam
        )

        all_rows.extend(rows)
        per_camera[cam] = (trajectories, velocities, frame_size)

    # ── Write output data ──────────────────────────────────────────────────────
    stem_prefix = session_dir / "motion_results"

    if args.out_format == "csv":
        out = stem_prefix.with_suffix(".csv")
        write_csv(all_rows, out)
        print(f"[motion] Wrote {out}", flush=True)
    elif args.out_format == "json":
        out = stem_prefix.with_suffix(".json")
        write_json(all_rows, out)
        print(f"[motion] Wrote {out}", flush=True)
    elif args.out_format == "hdf5":
        out = stem_prefix.with_suffix(".h5")
        write_hdf5(all_rows, out)
        print(f"[motion] Wrote {out}", flush=True)

    # ── Visualisations, one set per camera ─────────────────────────────────────
    for cam, (trajectories, velocities, frame_size) in sorted(per_camera.items()):
        label = f"{session_dir.name}, Camera {cam + 1}"
        if not args.no_heatmap and trajectories:
            out = str(session_dir / f"motion_heatmap_cam{cam}.png")
            generate_heatmap(trajectories, frame_size, out, title=f"Position heatmap: {label}")
            print(f"[motion] Wrote {out}", flush=True)
        if not args.no_trajectory and trajectories:
            out = str(session_dir / f"motion_trajectories_cam{cam}.png")
            generate_trajectory_plot(trajectories, frame_size, out, title=f"Trajectories: {label}")
            print(f"[motion] Wrote {out}", flush=True)
        if velocities:
            out = str(session_dir / f"motion_velocity_hist_cam{cam}.png")
            generate_velocity_histogram(
                velocities, out, mm_per_px=args.mm_per_px, title=f"Speeds: {label}"
            )
            print(f"[motion] Wrote {out}", flush=True)

    print(f"[motion] Done. {len(all_rows)} total detections.", flush=True)


# ── Single video mode ─────────────────────────────────────────────────────────


def process_single(args: argparse.Namespace) -> None:
    video_path = Path(args.video)
    out_dir = video_path.parent

    tracker = CentroidTracker(
        min_area=args.min_area,
        max_area=args.max_area,
        max_distance=args.max_dist,
        mm_per_px=args.mm_per_px,
        n_animals=args.n_animals,
    )

    timestamps = load_timestamps(args.timestamps)
    rows, trajectories, velocities, frame_size = process_video(
        video_path,
        tracker,
        timestamps,
        skip=args.skip,
        preview=args.preview,
        camera_index=camera_index_from_filename(video_path) or 0,
    )

    stem = out_dir / f"motion_{video_path.stem}"

    if args.out_format == "csv":
        write_csv(rows, stem.with_suffix(".csv"))
    elif args.out_format == "json":
        write_json(rows, stem.with_suffix(".json"))
    elif args.out_format == "hdf5":
        write_hdf5(rows, stem.with_suffix(".h5"))

    if not args.no_heatmap and trajectories:
        generate_heatmap(trajectories, frame_size, str(stem.parent / f"{stem.name}_heatmap.png"))
    if not args.no_trajectory and trajectories:
        generate_trajectory_plot(
            trajectories, frame_size, str(stem.parent / f"{stem.name}_trajectories.png")
        )
    if velocities:
        generate_velocity_histogram(
            velocities,
            str(stem.parent / f"{stem.name}_velocity_hist.png"),
            mm_per_px=args.mm_per_px,
        )

    print(f"[motion] Done. {len(rows)} detections.", flush=True)


# ── CLI ───────────────────────────────────────────────────────────────────────


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="MOSAIC mouse centroid motion tracker",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument("--session", metavar="DIR", help="Path to a MOSAIC session directory")
    group.add_argument("--video", metavar="FILE", help="Path to a single video file")

    p.add_argument("--n-animals", type=int, default=0, metavar="N")
    p.add_argument("--min-area", type=int, default=400, metavar="PX2")
    p.add_argument("--max-area", type=int, default=7000, metavar="PX2")
    p.add_argument("--max-dist", type=float, default=80.0, metavar="PX")
    p.add_argument("--mm-per-px", type=float, default=1.0, metavar="F")
    p.add_argument("--skip", type=int, default=1, metavar="N")
    p.add_argument("--out-format", choices=["csv", "json", "hdf5"], default="csv")
    p.add_argument("--timestamps", default=None, metavar="CSV")
    p.add_argument("--no-heatmap", action="store_true")
    p.add_argument("--no-trajectory", action="store_true")
    p.add_argument("--preview", action="store_true")
    return p


def main() -> None:
    args = build_parser().parse_args()
    if args.session:
        process_session(args)
    else:
        process_single(args)


if __name__ == "__main__":
    # Started from a terminal, Windows' console code page cannot print the
    # arrows and other characters in this script's messages; inside MOSAIC
    # PYTHONIOENCODING already makes it UTF-8.
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass
    main()
