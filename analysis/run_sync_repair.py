"""
MOSAIC Frame Sync Repair runner — equalizes every camera's frame count in a
recorded session by aligning all cameras to a shared master tick grid
(analysis/sync_repair/alignment.py) and filling small per-camera
frame-count mismatches (GVSP packet loss / trigger misses) by duplicating
the nearest-available frame. Writes per-camera repaired copies into a
sibling "synced/" folder, plus a per-camera audit-trail CSV marking which
output frames are duplicates and one small JSON summary. Never touches the
original recordings.

    python analysis/run_sync_repair.py --session /path/to/session_2026-06-07_14-30
    python analysis/run_sync_repair.py --session /path/to/session --master-fps 15.0

Explicit scope (stated, not silently dropped):
  - Video-only — audio is not touched/resampled by this feature.
  - No other analysis plugin (Pose, Expression, Face Masking, …) is
    switched to consume synced/ instead of video/ — they are completely
    unaffected; this is a pure additive/exportable repair pass, never a
    canonical-data replacement.
  - The session's canonical sync_manifest.json (used by SessionPlayerW at
    its own fixed 25fps default, and by Session Health) is never read or
    written here — see analysis/sync_repair/alignment.py's module doc for
    why, and AnalysisManager::run_sync_repair()'s doc comment on the C++
    side.

See analysis/README.rst for full documentation.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
from sync_repair.alignment import (
    AlignmentResult,
    CameraFrames,
    build_tick_grid,
    compute_master_fps,
)

_MAX_CAMERAS = 16  # matches SyncManifest::generate()'s own kMaxCams constant

# ── CLI ───────────────────────────────────────────────────────────────────────


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="MOSAIC Frame Sync Repair — equalize per-camera frame counts"
    )
    parser.add_argument(
        "--session", metavar="DIR", required=True, help="Recorded session directory"
    )
    parser.add_argument(
        "--master-fps",
        type=float,
        default=0.0,
        help="Uniform output frame rate for every camera's repaired video "
        "(default: 0 = auto, picks the fastest camera's own achieved fps in "
        "this session — never upsamples beyond what a real camera actually "
        "captured)",
    )
    return parser.parse_args()


# ── Camera discovery ─────────────────────────────────────────────────────────


@dataclass
class DiscoveredCamera:
    index: int
    video_path: Path | None
    csv_path: Path | None
    skipped: bool
    skip_reason: str | None


def _configured_camera_indices(session_dir: Path) -> list[int]:
    """Reads session_meta.json's own declared camera list — the real source
    of truth for which camera indices belong to this session at all,
    distinct from which of those actually produced usable output (checked
    separately, per index, by discover_cameras()). Falls back to scanning
    video/timestamps_cam*.csv for whatever indices exist on disk if
    session_meta.json is missing or has no cameras array — best effort, so
    a session missing its own meta file still gets *something* repaired
    rather than nothing.

    Deliberately NOT a "scan N=0.. until the first missing index" walk
    (unlike SyncManifest::generate()'s own kMaxCams scan) — that would
    silently stop discovering cameras at the first gap, incorrectly
    excluding any higher-indexed camera that DOES have real data if a
    lower-indexed one happens to be missing (not just the common
    last-camera-absent case this project has already seen in practice).
    """
    meta_path = session_dir / "session_meta.json"
    if meta_path.exists():
        try:
            meta = json.loads(meta_path.read_text())
            indices = sorted({int(c["index"]) for c in meta.get("cameras", []) if "index" in c})
            if indices:
                return indices
        except (json.JSONDecodeError, KeyError, ValueError, TypeError):
            pass

    video_dir = session_dir / "video"
    found: list[int] = []
    if video_dir.is_dir():
        for p in video_dir.glob("timestamps_cam*.csv"):
            m = re.search(r"timestamps_cam(\d+)\.csv$", p.name)
            if m:
                found.append(int(m.group(1)))
    return sorted(found)


def discover_cameras(session_dir: Path) -> list[DiscoveredCamera]:
    video_dir = session_dir / "video"
    result: list[DiscoveredCamera] = []
    for idx in _configured_camera_indices(session_dir):
        if idx < 0 or idx >= _MAX_CAMERAS:
            # Recorded as skipped rather than silently dropped, so an
            # out-of-range index shows up in sync_repair.json's camera list
            # with a reason — consistent with every other skip path below.
            result.append(
                DiscoveredCamera(
                    idx,
                    None,
                    None,
                    True,
                    f"camera index out of the supported range [0, {_MAX_CAMERAS})",
                )
            )
            continue
        video_path = video_dir / f"video_{idx}.mp4"
        csv_path = video_dir / f"timestamps_cam{idx}.csv"
        has_video = video_path.exists()
        has_csv = csv_path.exists()
        if has_video and has_csv:
            result.append(DiscoveredCamera(idx, video_path, csv_path, False, None))
        else:
            reasons = []
            if not has_video:
                reasons.append("no video_N.mp4 found")
            if not has_csv:
                reasons.append("no timestamps_camN.csv found")
            result.append(DiscoveredCamera(idx, None, None, True, "; ".join(reasons)))
    return result


def _read_camera_frames(csv_path: Path, camera_index: int) -> tuple[CameraFrames, dict[int, int]]:
    """Returns (CameraFrames for alignment, frame_id -> CSV row ordinal
    map). A row's ordinal position (0-based enumerate order over the file)
    is what maps 1:1 onto the source mp4's frame sequence — NOT the
    frame_id value itself, which can in principle have gaps (VideoGrabber
    assigns frame_id before the ring-buffer push, so a dropped frame
    consumes a frame_id that never reaches this CSV or the mp4 — see
    analysis/sync_repair/alignment.py's module doc)."""
    frame_ids: list[int] = []
    elapsed_ns: list[int] = []
    ordinal_map: dict[int, int] = {}
    with csv_path.open(newline="") as f:
        for ordinal, row in enumerate(csv.DictReader(f)):
            try:
                fid = int(row["frame_id"])
                ens = int(row["elapsed_ns"])
            except (KeyError, ValueError, TypeError):
                continue
            frame_ids.append(fid)
            elapsed_ns.append(ens)
            ordinal_map[fid] = ordinal
    return (
        CameraFrames(
            index=camera_index,
            frame_ids=np.array(frame_ids, dtype=np.int64),
            elapsed_ns=np.array(elapsed_ns, dtype=np.int64),
        ),
        ordinal_map,
    )


# ── Per-camera repair (single sequential forward pass) ──────────────────────


def _open_writer(out_path: Path, fps: float, size: tuple[int, int]):
    # avc1 (H.264) plays back more reliably in Qt/Windows Media Foundation
    # than mp4v, but isn't always available depending on the OpenCV build's
    # bundled FFmpeg — fall back to mp4v (always available) if it fails.
    # Copied verbatim from run_face_mask.py's own already-proven helper.
    for fourcc_name in ("avc1", "mp4v"):
        fourcc = cv2.VideoWriter_fourcc(*fourcc_name)
        writer = cv2.VideoWriter(str(out_path), fourcc, fps, size)
        if writer.isOpened():
            return writer
        writer.release()
    return None


def _repair_camera(
    video_path: Path,
    ordinal_map: dict[int, int],
    assigned_frame_ids: np.ndarray,
    out_video_path: Path,
    out_csv_path: Path,
    master_fps: float,
) -> tuple[int, int, str | None]:
    """Writes out_video_path (exactly len(assigned_frame_ids) frames, one
    per master tick) and out_csv_path (its per-tick audit trail) via a
    SINGLE sequential forward pass over video_path — never seeks backward,
    never uses CAP_PROP_POS_FRAMES. Correct because assigned_frame_ids is
    guaranteed non-decreasing across ticks by construction (see
    build_tick_grid()'s own doc comment).

    Returns (output_frame_count, duplicated_frame_count, note_or_None).
    note is set only if the source video ended earlier than its own CSV
    claimed (a truncated/corrupt file) — the remaining ticks are then
    filled by freezing on the last successfully-decoded frame, always
    marked duplicated in the CSV (the pixel content genuinely didn't
    change), never silently treated as fresh data.
    """
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        return 0, 0, f"could not open {video_path.name} for reading"

    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    writer = _open_writer(out_video_path, master_fps, (width, height))
    if writer is None:
        cap.release()
        return 0, 0, f"could not open a video writer for {out_video_path.name}"

    total_ticks = len(assigned_frame_ids)
    progress_interval = max(1, total_ticks // 200)

    frames_read = -1  # ordinal of the last frame successfully decoded so far
    current_frame: np.ndarray | None = None
    truncated = False
    truncated_at_ordinal = -1
    duplicated_count = 0
    prev_frame_id: int | None = None
    rows: list[tuple[int, int, bool]] = []
    t_start = time.perf_counter()

    for tick in range(total_ticks):
        frame_id = int(assigned_frame_ids[tick])
        target_ordinal = ordinal_map.get(frame_id)

        if target_ordinal is None and not truncated:
            # Should not happen — every assigned frame_id came from this
            # same camera's own CSV (see build_tick_grid()). Defensive
            # fallback: treat exactly like a truncated read, never crash.
            # truncated_at_ordinal is still recorded here (frames_read
            # reflects however many frames were genuinely decoded in prior
            # ticks) so the note below never under-reports how far the
            # camera actually got, regardless of which branch tripped it.
            truncated = True
            truncated_at_ordinal = frames_read

        if not truncated and target_ordinal is not None:
            while frames_read < target_ordinal:
                ok, frame = cap.read()
                if not ok:
                    truncated = True
                    truncated_at_ordinal = frames_read
                    break
                frames_read += 1
                current_frame = frame

        if current_frame is None:
            # Never successfully decoded a single frame — write a
            # correctly-sized black frame rather than crash; the returned
            # note flags this camera's whole output as unreliable.
            current_frame = np.zeros((height, width, 3), dtype=np.uint8)

        is_duplicate = truncated or (prev_frame_id is not None and frame_id == prev_frame_id)
        writer.write(current_frame)
        if is_duplicate:
            duplicated_count += 1
        rows.append((tick, frame_id, is_duplicate))
        prev_frame_id = frame_id

        if (tick + 1) % progress_interval == 0:
            elapsed = time.perf_counter() - t_start
            pct = (tick + 1) / max(total_ticks, 1) * 100
            print(f"  {pct:5.1f}%  ({tick + 1}/{total_ticks})  {elapsed:.1f}s elapsed", flush=True)

    cap.release()
    writer.release()

    with out_csv_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["output_frame_index", "source_frame_id", "duplicated"])
        for out_idx, fid, dup in rows:
            w.writerow([out_idx, fid, "true" if dup else "false"])

    note = None
    if truncated:
        note = (
            f"source video ended early (only {max(truncated_at_ordinal, 0) + 1} of an "
            f"expected {len(ordinal_map)} frame(s) could be decoded) — remaining ticks "
            f"were filled by duplicating the last successfully-decoded frame"
        )

    return len(rows), duplicated_count, note


# ── Session mode ─────────────────────────────────────────────────────────────


def process_session(session_dir: Path, master_fps_arg: float) -> None:
    discovered = discover_cameras(session_dir)
    if not discovered:
        print(
            f"[run_sync_repair] No cameras found for {session_dir} — no session_meta.json "
            f"cameras[] entries and no timestamps_cam*.csv files.",
            file=sys.stderr,
        )
        sys.exit(1)

    for d in discovered:
        if d.skipped:
            print(f"[run_sync_repair] Camera {d.index}: skipped — {d.skip_reason}", file=sys.stderr)

    present = [d for d in discovered if not d.skipped]
    if not present:
        print(
            "[run_sync_repair] No camera has both a video file and a timestamps file "
            "— nothing to repair.",
            file=sys.stderr,
        )
        sys.exit(1)

    cam_frames: dict[int, CameraFrames] = {}
    ordinal_maps: dict[int, dict[int, int]] = {}
    source_counts: dict[int, int] = {}
    for d in present:
        cf, om = _read_camera_frames(d.csv_path, d.index)
        cam_frames[d.index] = cf
        ordinal_maps[d.index] = om
        source_counts[d.index] = len(cf.frame_ids)
        if len(cf.frame_ids) == 0:
            d.skipped = True
            d.skip_reason = "timestamps file has zero data rows"

    present = [d for d in present if not d.skipped]
    if not present:
        print(
            "[run_sync_repair] Every usable camera's timestamps file was empty "
            "— nothing to repair.",
            file=sys.stderr,
        )
        sys.exit(1)

    print(
        f"[run_sync_repair] Found {len(present)}/{len(discovered)} usable camera(s) "
        f"in {session_dir}",
        flush=True,
    )

    if master_fps_arg > 0.0:
        master_fps = master_fps_arg
        print(f"[run_sync_repair] Using explicit master fps: {master_fps:.3f}", flush=True)
    else:
        master_fps = compute_master_fps(list(cam_frames.values()))
        if master_fps <= 0.0:
            print(
                "[run_sync_repair] Could not auto-detect a master fps (every camera "
                "captured fewer than 2 frames) — pass --master-fps explicitly.",
                file=sys.stderr,
            )
            sys.exit(1)
        print(
            f"[run_sync_repair] Auto-detected master fps: {master_fps:.3f} (fastest camera)",
            flush=True,
        )

    grid: AlignmentResult = build_tick_grid(list(cam_frames.values()), master_fps)
    print(
        f"[run_sync_repair] {grid.total_ticks} tick(s) @ {master_fps:.3f} fps "
        f"({grid.total_ticks / master_fps:.1f}s)",
        flush=True,
    )

    out_dir = session_dir / "synced"
    out_dir.mkdir(parents=True, exist_ok=True)

    summary_cameras: dict[int, dict] = {
        d.index: {
            "index": d.index,
            "source_video": None,
            "repaired_video": None,
            "source_frames_captured": source_counts.get(d.index, 0),
            "output_frame_count": 0,
            "duplicated_frame_count": 0,
            "skipped": True,
            "skip_reason": d.skip_reason,
            "note": None,
        }
        for d in discovered
        if d.skipped
    }

    failures = 0
    for position, d in enumerate(present, start=1):
        print(
            f"[run_sync_repair] Camera {position}/{len(present)}: {d.video_path.name}",
            flush=True,
        )

        video_rel = f"video/{d.video_path.name}"
        out_video = out_dir / d.video_path.name
        out_csv = out_dir / f"{d.video_path.stem}.repair_map.csv"

        output_count, dup_count, note = _repair_camera(
            d.video_path,
            ordinal_maps[d.index],
            grid.frame_ids_by_camera[d.index],
            out_video,
            out_csv,
            master_fps,
        )

        if output_count != grid.total_ticks:
            print(
                f"[run_sync_repair] Camera {d.index}: produced {output_count} output "
                f"frame(s), expected {grid.total_ticks} — treating as failed.",
                file=sys.stderr,
            )
            out_video.unlink(missing_ok=True)
            out_csv.unlink(missing_ok=True)
            failures += 1
            summary_cameras[d.index] = {
                "index": d.index,
                "source_video": video_rel,
                "repaired_video": None,
                "source_frames_captured": source_counts[d.index],
                "output_frame_count": 0,
                "duplicated_frame_count": 0,
                "skipped": True,
                "skip_reason": "repair produced an unexpected output frame count (see log)",
                "note": note,
            }
            continue

        summary_cameras[d.index] = {
            "index": d.index,
            "source_video": video_rel,
            "repaired_video": f"synced/{d.video_path.name}",
            "source_frames_captured": source_counts[d.index],
            "output_frame_count": output_count,
            "duplicated_frame_count": dup_count,
            "skipped": False,
            "skip_reason": None,
            "note": note,
        }
        print(
            f"[run_sync_repair] Done. Camera {d.index}: {output_count} frame(s), "
            f"{dup_count} duplicated -> synced/{d.video_path.name}",
            flush=True,
        )

    summary = {
        "schema": "mosaic-sync-repair-v1",
        "master_fps": master_fps,
        "total_ticks": grid.total_ticks,
        "duration_ms": round(grid.total_ticks / master_fps * 1000),
        "generated_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "cameras": [summary_cameras[idx] for idx in sorted(summary_cameras)],
    }
    (out_dir / "sync_repair.json").write_text(json.dumps(summary, indent=2))
    print(f"[run_sync_repair] Summary -> {out_dir / 'sync_repair.json'}", flush=True)

    n_ok = sum(1 for c in summary_cameras.values() if not c["skipped"])
    if n_ok == 0:
        print(
            "[run_sync_repair] Every camera failed or was skipped — nothing was repaired.",
            file=sys.stderr,
        )
        sys.exit(1)
    if failures:
        print(
            f"[run_sync_repair] {failures}/{len(present)} camera(s) failed — see errors above.",
            file=sys.stderr,
        )
        sys.exit(1)


# ── Entry point ───────────────────────────────────────────────────────────────


def main() -> None:
    args = parse_args()
    process_session(Path(args.session), args.master_fps)


if __name__ == "__main__":
    main()
