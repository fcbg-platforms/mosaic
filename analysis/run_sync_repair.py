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

Alignment: when the recording logged its trigger ticks
(video/action_ticks.csv + video/action_group.json — every hardware-triggered
recording since they were added), each frame is placed on the tick that
produced it (sync_repair/tick_alignment.py) and the output has one frame per
tick over the window every camera was running in. Otherwise — older sessions,
free-running cameras, an explicit --master-fps — frames are placed on a grid
by arrival time, as before (sync_repair/alignment.py). Either way every
camera's output has the same frame count, and a tick a camera has no frame
for repeats its last real frame with a red MISSING tag (sync_repair/marker.py).

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
from sync_repair.marker import mark_missing
from sync_repair.tick_alignment import (
    EXPOSURE_MIN_SHARE,
    TickCamera,
    build_tick_plan,
    gap_ranges,
)

_MAX_GAPS_LISTED = 100  # per camera in sync_repair.json; the total is always given

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


def _read_camera_frames(
    csv_path: Path, camera_index: int
) -> tuple[CameraFrames, dict[int, int], np.ndarray, np.ndarray]:
    """Returns (CameraFrames for alignment, frame_id -> CSV row ordinal
    map, hardware timestamps, exposures in ns; 0 = unavailable in both).
    A row's ordinal position (0-based enumerate order over the file)
    is what maps 1:1 onto the source mp4's frame sequence — NOT the
    frame_id value itself, which can in principle have gaps (VideoGrabber
    assigns frame_id before the ring-buffer push, so a dropped frame
    consumes a frame_id that never reaches this CSV or the mp4 — see
    analysis/sync_repair/alignment.py's module doc)."""
    frame_ids: list[int] = []
    elapsed_ns: list[int] = []
    hw_ns: list[int] = []
    exposure_ns: list[int] = []
    ordinal_map: dict[int, int] = {}
    with csv_path.open(newline="") as f:
        for ordinal, row in enumerate(csv.DictReader(f)):
            try:
                fid = int(row["frame_id"])
                ens = int(row["elapsed_ns"])
            except (KeyError, ValueError, TypeError):
                continue
            # Older files have no hardware column; 0 means "unavailable".
            try:
                hw = int(row.get("hw_timestamp_ns") or 0)
            except (ValueError, TypeError):
                hw = 0
            # Likewise exposure (µs, empty when the camera did not report it).
            try:
                exp = round(float(row.get("exposure_us") or 0) * 1000)
            except (ValueError, TypeError, OverflowError):  # "inf" overflows round()
                exp = 0
            frame_ids.append(fid)
            elapsed_ns.append(ens)
            hw_ns.append(hw)
            exposure_ns.append(max(exp, 0))
            ordinal_map[fid] = ordinal
    return (
        CameraFrames(
            index=camera_index,
            frame_ids=np.array(frame_ids, dtype=np.int64),
            elapsed_ns=np.array(elapsed_ns, dtype=np.int64),
        ),
        ordinal_map,
        np.array(hw_ns, dtype=np.int64),
        np.array(exposure_ns, dtype=np.int64),
    )


def _load_trigger_ticks(session_dir: Path) -> tuple[np.ndarray, set[int]] | None:
    """The recording's trigger tick times and the configured indices of the
    cameras they apply to, or None when the session has no usable tick log
    (older sessions, free-running rigs, interview mode)."""
    video_dir = session_dir / "video"
    log_path = video_dir / "action_ticks.csv"
    group_path = video_dir / "action_group.json"
    if not log_path.exists() or not group_path.exists():
        return None
    try:
        group = json.loads(group_path.read_text())
        cameras = {int(c) for c in group.get("cameras", [])}
    except (ValueError, TypeError, OSError):
        return None
    times: list[int] = []
    with log_path.open(newline="") as f:
        for expected_tick, row in enumerate(csv.DictReader(f)):
            # Stop at the first row that is not a complete, consecutive,
            # later tick. A crash — or a ticker thread killed mid-write —
            # leaves a cut-off last line, and a cut-off number still parses
            # ("1234" of "123456789"): only the row's shape and order say it
            # is wrong. Everything before it is intact.
            try:
                tick = int(row["tick"])
                t = int(row["elapsed_ns"])
                int(row["fired"])
            except (KeyError, ValueError, TypeError):
                break
            if tick != expected_tick or (times and t <= times[-1]):
                break
            times.append(t)
    if len(times) < 2 or not cameras:
        return None
    return np.array(times, dtype=np.int64), cameras


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
    missing: np.ndarray | None = None,
    first_tick: int | None = None,
) -> tuple[int, int, str | None]:
    """Writes out_video_path (exactly len(assigned_frame_ids) frames, one
    per master tick) and out_csv_path (its per-tick audit trail) via a
    SINGLE sequential forward pass over video_path — never seeks backward,
    never uses CAP_PROP_POS_FRAMES. Correct because assigned_frame_ids is
    guaranteed non-decreasing across ticks by construction (see
    build_tick_grid()'s own doc comment).

    `missing` marks output frames that repeat an earlier frame because the
    camera has none for that tick; they carry a red MISSING tag. Without it
    (arrival-time alignment), a repeat of the previous tick's frame is the
    same thing and is tagged the same way. `first_tick` numbers the repair
    map's `tick` column when the output is on trigger ticks.

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

        is_missing = (
            bool(missing[tick])
            if missing is not None
            else (prev_frame_id is not None and frame_id == prev_frame_id)
        )
        is_duplicate = truncated or is_missing
        # Tagged so a repeat is never mistaken for a fresh image — see
        # sync_repair/marker.py.
        writer.write(mark_missing(current_frame) if is_duplicate else current_frame)
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
        # `missing` is the newer name for the same flag; `duplicated` is kept
        # for readers of older maps. `tick` is the trigger tick (empty when
        # aligned on arrival time).
        w.writerow(["output_frame_index", "source_frame_id", "duplicated", "missing", "tick"])
        for out_idx, fid, dup in rows:
            flag = "true" if dup else "false"
            tick_col = "" if first_tick is None else first_tick + out_idx
            w.writerow([out_idx, fid, flag, flag, tick_col])

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
            print(
                f"[run_sync_repair] Camera {d.index + 1} (video_{d.index}.mp4): skipped — "
                f"{d.skip_reason}",
                file=sys.stderr,
            )

    present = [d for d in discovered if not d.skipped]
    if not present:
        print(
            "[run_sync_repair] No camera has both a video file and a timestamps file "
            "— nothing to repair.",
            file=sys.stderr,
        )
        sys.exit(1)

    cam_frames: dict[int, CameraFrames] = {}
    hw_by_camera: dict[int, np.ndarray] = {}
    exposure_by_camera: dict[int, np.ndarray] = {}
    ordinal_maps: dict[int, dict[int, int]] = {}
    source_counts: dict[int, int] = {}
    for d in present:
        cf, om, hw, exp = _read_camera_frames(d.csv_path, d.index)
        cam_frames[d.index] = cf
        hw_by_camera[d.index] = hw
        exposure_by_camera[d.index] = exp
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

    # Per camera: the frame each output tick shows, which output frames are
    # missing, how it was aligned, and the trigger-tick extras for the report.
    per_camera: dict[int, dict] = {}
    alignment = "arrival_time"
    first_tick: int | None = None

    trigger = None if master_fps_arg > 0.0 else _load_trigger_ticks(session_dir)
    plan = None
    if trigger is not None:
        tick_times, group = trigger
        group_cams = [
            TickCamera(
                index=i,
                frame_ids=cam_frames[i].frame_ids,
                elapsed_ns=cam_frames[i].elapsed_ns,
                hw_ns=hw_by_camera[i],
                exposure_ns=exposure_by_camera[i],
            )
            for i in sorted(cam_frames)
            if i in group
        ]
        plan = build_tick_plan(tick_times, group_cams)
        if plan is None:
            print(
                "[run_sync_repair] Trigger tick log present but unusable — aligning on "
                "arrival time instead.",
                flush=True,
            )

    if plan is not None:
        alignment = "trigger_ticks"
        first_tick = plan.first_tick
        master_fps = plan.tick_rate_fps
        total_ticks = plan.total_ticks
        window_times = tick_times[plan.first_tick : plan.first_tick + plan.total_ticks]
        latencies = [
            c.median_latency_ms for c in plan.cameras.values() if c.median_latency_ms is not None
        ]
        typical_latency_ns = float(np.median(latencies)) * 1e6 if latencies else 0.0
        # When the plan took each frame's exposure off, those latencies leave
        # it out: a free-running camera below is then matched on its arrival
        # times less its *own* exposure, or, if it reports none, with the
        # triggered cameras' typical exposure added back to the latency.
        group_exposure_ns = 0.0
        if plan.exposure_corrected:
            exposures = np.concatenate([exposure_by_camera[i] for i in plan.cameras])
            exposures = exposures[exposures > 0]
            if len(exposures):
                group_exposure_ns = float(np.median(exposures))
        for i, cam in plan.cameras.items():
            per_camera[i] = {
                "frame_ids": cam.frame_ids,
                "missing": cam.missing,
                "alignment": cam.method,
                "lead_in_trimmed": cam.lead_in_trimmed,
                "tail_trimmed": cam.tail_trimmed,
                "alignment_uncertain": cam.uncertain,
                "median_latency_ms": cam.median_latency_ms,
                "joined_late_at": cam.joined_late_at,
                "dropped_out_at": cam.dropped_out_at,
            }
        # A camera outside the Action group free-runs: it never followed these
        # ticks, so it gets the frame that arrived nearest each tick's expected
        # arrival time, and a repeat where none is new. Only cameras still in
        # the run — one whose timestamps file was empty is already skipped.
        for i in sorted(d.index for d in present):
            if i in per_camera:
                continue
            cf = cam_frames[i]
            fallback_note = (
                "in the trigger group but delivered fewer than two frames — placed by "
                "arrival time"
                if i in group
                else None
            )
            arrival = cf.elapsed_ns
            latency_ns = typical_latency_ns
            if plan.exposure_corrected:
                own = exposure_by_camera[i]
                if len(own) and (own > 0).mean() >= EXPOSURE_MIN_SHARE:
                    fill = np.where(own > 0, own, int(np.median(own[own > 0])))
                    arrival = cf.elapsed_ns - fill
                else:
                    latency_ns += group_exposure_ns
            ids = np.empty(total_ticks, dtype=np.int64)
            ptr = 0
            for out in range(total_ticks):
                target = float(window_times[out]) + latency_ns
                while ptr + 1 < len(arrival) and abs(arrival[ptr + 1] - target) < abs(
                    arrival[ptr] - target
                ):
                    ptr += 1
                ids[out] = cf.frame_ids[ptr]
            rep = np.zeros(total_ticks, dtype=bool)
            rep[1:] = ids[1:] == ids[:-1]
            per_camera[i] = {
                "frame_ids": ids,
                "missing": rep,
                "alignment": "arrival_time",
                "lead_in_trimmed": None,
                "tail_trimmed": None,
                "alignment_uncertain": False,
                "median_latency_ms": None,
                "fallback_note": fallback_note,
            }
        # The ticker re-paces itself as the cameras' measured rates settle, so
        # the tick rate can change during a recording. The output is a
        # constant-rate video (one frame per tick), so a changing rate plays
        # parts of it slightly fast or slow — frame k still means tick k.
        # Said, not hidden.
        if plan.max_tick_rate_fps > plan.min_tick_rate_fps * 1.02:
            print(
                f"[run_sync_repair] Note: the trigger rate varied between "
                f"{plan.min_tick_rate_fps:.2f} and {plan.max_tick_rate_fps:.2f} fps during this "
                f"recording; the synced videos play at {master_fps:.2f} fps throughout, so "
                f"stretches recorded at another rate play slightly fast or slow. Frame k is "
                f"still trigger tick k in every camera.",
                flush=True,
            )
        print(
            f"[run_sync_repair] Aligned on {total_ticks} trigger tick(s) @ "
            f"{master_fps:.3f} fps ({total_ticks / master_fps:.1f}s)",
            flush=True,
        )

    if plan is None and master_fps_arg > 0.0:
        master_fps = master_fps_arg
        print(f"[run_sync_repair] Using explicit master fps: {master_fps:.3f}", flush=True)
    elif plan is None:
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

    if plan is None:
        grid: AlignmentResult = build_tick_grid(list(cam_frames.values()), master_fps)
        total_ticks = grid.total_ticks
        for i, ids in grid.frame_ids_by_camera.items():
            rep = np.zeros(len(ids), dtype=bool)
            rep[1:] = ids[1:] == ids[:-1]
            per_camera[i] = {
                "frame_ids": ids,
                "missing": rep,
                "alignment": "arrival_time",
                "lead_in_trimmed": None,
                "tail_trimmed": None,
                "alignment_uncertain": False,
                "median_latency_ms": None,
            }
        print(
            f"[run_sync_repair] {total_ticks} tick(s) @ {master_fps:.3f} fps "
            f"({total_ticks / master_fps:.1f}s), aligned on arrival time",
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

        pc = per_camera[d.index]
        output_count, dup_count, note = _repair_camera(
            d.video_path,
            ordinal_maps[d.index],
            pc["frame_ids"],
            out_video,
            out_csv,
            master_fps,
            missing=pc["missing"],
            first_tick=first_tick if pc["alignment"].startswith("trigger_ticks") else None,
        )

        if output_count != total_ticks:
            print(
                f"[run_sync_repair] Camera {d.index + 1} ({d.video_path.name}): "
                f"produced {output_count} output "
                f"frame(s), expected {total_ticks} — treating as failed.",
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

        gaps = gap_ranges(pc["missing"])
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
            # How this camera was lined up, and what it is missing.
            "alignment": pc["alignment"],
            "missing_frame_count": int(np.sum(pc["missing"])),
            "gap_count": len(gaps),
            "gaps": [list(g) for g in gaps[:_MAX_GAPS_LISTED]],
            "lead_in_trimmed": pc["lead_in_trimmed"],
            "tail_trimmed": pc["tail_trimmed"],
            "alignment_uncertain": pc["alignment_uncertain"],
            "median_latency_ms": pc["median_latency_ms"],
            # Output frame where it started delivering / stopped, when it
            # joined or dropped out far from the others; else null.
            "joined_late_at": pc.get("joined_late_at"),
            "dropped_out_at": pc.get("dropped_out_at"),
        }
        if pc.get("fallback_note"):
            existing = summary_cameras[d.index]["note"]
            summary_cameras[d.index]["note"] = (
                pc["fallback_note"] if not existing else f"{existing}; {pc['fallback_note']}"
            )
        print(
            f"[run_sync_repair] Done. Camera {d.index + 1} ({d.video_path.name}): "
            f"{output_count} frame(s), "
            f"{dup_count} duplicated -> synced/{d.video_path.name}",
            flush=True,
        )

    summary = {
        "schema": "mosaic-sync-repair-v1",
        "alignment": alignment,
        "master_fps": master_fps,
        "tick_rate_fps": master_fps if alignment == "trigger_ticks" else None,
        "min_tick_rate_fps": plan.min_tick_rate_fps if plan is not None else None,
        "max_tick_rate_fps": plan.max_tick_rate_fps if plan is not None else None,
        "first_tick": first_tick,
        "total_ticks": total_ticks,
        # Whether each frame's own exposure was taken off its arrival time
        # before placing it on a tick (needs exposure_us in every camera's
        # timestamps file). Latencies are then trigger-to-arrival less it.
        "exposure_corrected": plan.exposure_corrected if plan is not None else False,
        # The real span of the recording on trigger ticks, not frames / rate,
        # which is off whenever the tick rate changed during it.
        "duration_ms": round(plan.duration_ms)
        if plan is not None
        else round(total_ticks / master_fps * 1000),
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
