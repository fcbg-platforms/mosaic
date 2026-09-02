"""
Pure, self-contained tick-grid alignment for MOSAIC's Frame Sync Repair
plugin — deliberately REIMPLEMENTS the identical nearest-neighbor
tick-assignment algorithm SyncManifest::generate() already implements in
C++ (src/analysis/sync_manifest.cpp, the `while (ptr + 1 < nf)` walk),
rather than depending on this file's own inability to call into that C++
class directly (Python analysis scripts in this codebase never call into
C++ — they only ever read/write plain files).

**This module must never write into sync_manifest.json.** That file is the
app's canonical, already-shared alignment artifact — SessionPlayerW's
frame-step sizing and Session Health's per-camera sync stats both depend on
it, always generated at a fixed 25fps default (see MainWindow::
show_session_health()). This plugin's own master_fps can legitimately
differ from that default (see compute_master_fps() below) — persisting a
different rate under that filename would silently change SessionPlayerW's
already-shipped behavior. See run_sync_repair.py's own module docstring and
AnalysisManager::run_sync_repair()'s doc comment for the full reasoning.

If SyncManifest::generate()'s C++ tick-assignment loop is ever changed,
build_tick_grid() below should be reviewed for the same change, and vice
versa — this project's established pattern for algorithms duplicated
across the C++/Python boundary (e.g. the iris-offset gaze heuristic,
room_frame::invert() vs. analysis/pose3d/triangulation.py's invert_rt()).
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np


@dataclass
class CameraFrames:
    """One camera's sorted, 1:1-paired (frame_id, elapsed_ns) samples, read
    from its own timestamps_camN.csv. `index` is that camera's own
    configured index (matches "video_N.mp4"'s N) — not a position in
    whatever list this is passed around in."""

    index: int
    frame_ids: np.ndarray  # int64, ascending elapsed_ns order
    elapsed_ns: np.ndarray  # int64, same order, same length as frame_ids


@dataclass
class AlignmentResult:
    """The master tick grid and every non-empty camera's per-tick assigned
    frame_id — the direct answer to "for master tick T, camera C should
    show frame_id F", mirroring SyncManifest::frame_at_tick()'s return
    value exactly."""

    t_origin_ns: int
    step_ns: int
    total_ticks: int
    master_fps: float
    # camera.index -> per-tick assigned frame_id (length == total_ticks).
    frame_ids_by_camera: dict[int, np.ndarray] = field(default_factory=dict)


def compute_master_fps(cameras: list[CameraFrames]) -> float:
    """Auto master_fps = the FASTEST camera's own achieved fps in this
    session — deliberately the maximum across cameras, not an average or
    the slowest, so the repaired output never invents more frames than any
    real camera actually captured; it only fills the slower cameras up to
    match. Same formula SyncManifest uses for CameraSync::fpsActual.

    Cameras with fewer than 2 frames (no elapsed-time span to measure a
    rate from) are excluded, avoiding a division by zero. Returns 0.0 if no
    camera qualifies — the caller must treat that as "auto is undecidable"
    (e.g. every camera captured at most one frame) rather than pass 0.0
    into build_tick_grid(), which raises on a non-positive fps.
    """
    best = 0.0
    for cam in cameras:
        n = len(cam.frame_ids)
        if n < 2:
            continue
        span_ns = int(cam.elapsed_ns[-1]) - int(cam.elapsed_ns[0])
        if span_ns <= 0:
            continue
        fps_actual = (n - 1) * 1e9 / span_ns
        best = max(best, fps_actual)
    return best


def compute_window(cameras: list[CameraFrames]) -> tuple[int, int]:
    """Reimplements SyncManifest::generate()'s overlap-window (t_origin =
    latest first-frame elapsed_ns, t_end = earliest last-frame elapsed_ns —
    the window every camera is simultaneously live in) with its
    union-window fallback (t_origin = earliest first frame, t_end = latest
    last frame) if the cameras don't actually overlap (e.g. one camera
    stopped very early relative to the others).

    Raises ValueError if every camera has zero frames — callers must
    filter to non-empty cameras (or check first) before calling this.
    """
    nonempty = [c for c in cameras if len(c.elapsed_ns) > 0]
    if not nonempty:
        raise ValueError("compute_window() needs at least one camera with >= 1 frame")

    t_origin = max(int(c.elapsed_ns[0]) for c in nonempty)
    t_end = min(int(c.elapsed_ns[-1]) for c in nonempty)
    if t_end <= t_origin:
        t_origin = min(int(c.elapsed_ns[0]) for c in nonempty)
        t_end = max(int(c.elapsed_ns[-1]) for c in nonempty)
    return t_origin, t_end


def build_tick_grid(cameras: list[CameraFrames], master_fps: float) -> AlignmentResult:
    """Assigns every master tick a nearest-neighbor frame_id per camera,
    exactly mirroring SyncManifest::generate()'s C++ tick-walk loop (see
    this module's own docstring). When a camera's next real frame is far
    away, consecutive ticks resolve to the SAME frame_id (hold-nearest) —
    this IS the gap-filling/duplicate-marking signal the caller needs: a
    tick's assigned frame_id equalling the previous tick's assigned
    frame_id for that camera means "this output frame duplicates the
    previous one".

    Cameras with zero frames are silently excluded from the returned grid
    (nothing to assign) — callers wanting a "skipped" report for those must
    track that separately (run_sync_repair.py's own camera-discovery step
    already filters out cameras missing a video/CSV before this is ever
    called, and records why in sync_repair.json).

    Raises ValueError if master_fps <= 0.0 (an unresolved "auto" sentinel
    must never reach here — see compute_master_fps()'s own doc comment) or
    if no camera has any frames at all.
    """
    if master_fps <= 0.0:
        raise ValueError("build_tick_grid() needs a resolved, positive master_fps")

    nonempty = [c for c in cameras if len(c.elapsed_ns) > 0]
    t_origin, t_end = compute_window(nonempty)

    step_ns = int(round(1e9 / master_fps))
    total_ticks = max(1, (t_end - t_origin) // step_ns + 1)

    result = AlignmentResult(
        t_origin_ns=t_origin,
        step_ns=step_ns,
        total_ticks=total_ticks,
        master_fps=master_fps,
    )

    for cam in nonempty:
        n = len(cam.elapsed_ns)
        assigned = np.empty(total_ticks, dtype=np.int64)
        ptr = 0
        for tick in range(total_ticks):
            t_ideal = t_origin + tick * step_ns
            # Advance ptr while the NEXT frame is closer to t_ideal — the
            # same monotonic-pointer nearest-neighbor walk as the C++ loop.
            # Never resets/rewinds: assigned frame_ids are non-decreasing
            # across ticks for a given camera, which is exactly what lets
            # run_sync_repair.py read each camera's source video with a
            # single sequential forward pass (no seeking).
            while ptr + 1 < n:
                d_cur = abs(int(cam.elapsed_ns[ptr]) - t_ideal)
                d_next = abs(int(cam.elapsed_ns[ptr + 1]) - t_ideal)
                if d_next < d_cur:
                    ptr += 1
                else:
                    break
            assigned[tick] = cam.frame_ids[ptr]
        result.frame_ids_by_camera[cam.index] = assigned

    return result
