"""
Which frame of which video belongs to which tick.

Two sources, in order of preference:

1. **synced/** (Frame Sync Repair): every camera's ``synced/video_N.mp4`` has
   one frame per tick, so frame ``k`` of every camera is tick ``k``. Frames
   the repair filled in for a camera that missed a tick are flagged in
   ``video_N.repair_map.csv`` (``missing``) and are never analysed: they
   repeat an older image. Tick times come from the trigger tick log
   (``video/action_ticks.csv``, row = tick) when the repair aligned on
   trigger ticks, else from each source frame's own timestamp.
2. **Raw** ``video/`` with ``sync_manifest.json``: the manifest names the
   source frame nearest each tick. A frame further than half a tick from its
   tick is treated as missing rather than used out of time.

All file parsing, no video decoding: unit-tested on small fixtures.
"""

from __future__ import annotations

import csv
import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np


@dataclass
class CameraTrack:
    """One camera's frames along the timeline.

    Attributes
    ----------
    index : int
        Configured camera index (``video_N``).
    video_path : pathlib.Path
        Video to decode.
    frame_index : numpy.ndarray
        ``(T,)`` frame number in ``video_path`` for each tick, ``-1`` when
        this camera has no frame for the tick.
    missing : numpy.ndarray
        ``(T,)`` bool, True where the frame must not be analysed (a repeated
        frame, or none at all). Such frames are still drawn in videos.
    """

    index: int
    video_path: Path
    frame_index: np.ndarray
    missing: np.ndarray


@dataclass
class Timeline:
    """The tick grid shared by every camera.

    Attributes
    ----------
    source : str
        ``"synced"`` or ``"raw"``.
    times_ns : numpy.ndarray
        ``(T,)`` tick times on the recording's ``elapsed_ns`` clock.
    fps : float
        Nominal tick rate (also the output video frame rate).
    cameras : dict of int to CameraTrack
    first_tick : int or None
        Trigger tick of output frame 0 (synced, trigger-aligned only).
    """

    source: str
    times_ns: np.ndarray
    fps: float
    cameras: dict
    first_tick: int | None = None

    @property
    def n_ticks(self) -> int:
        return int(len(self.times_ns))


def _read_timestamps(csv_path: Path) -> tuple[np.ndarray, np.ndarray]:
    """``(frame_ids, elapsed_ns)`` in row order (= decode order)."""
    ids, times = [], []
    with csv_path.open(newline="") as f:
        for row in csv.DictReader(f):
            try:
                ids.append(int(row["frame_id"]))
                times.append(int(row["elapsed_ns"]))
            except (KeyError, TypeError, ValueError):
                continue
    return np.asarray(ids, dtype=np.int64), np.asarray(times, dtype=np.int64)


def _read_tick_log(path: Path) -> np.ndarray | None:
    if not path.exists():
        return None
    times = []
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            try:
                times.append(int(row["elapsed_ns"]))
            except (KeyError, TypeError, ValueError):
                break  # a cut-off last line ends the usable log
    return np.asarray(times, dtype=np.int64)


def load_synced(session: Path) -> Timeline | None:
    """Timeline from ``synced/``, or ``None`` when there is no usable repair."""
    synced = session / "synced"
    report_path = synced / "sync_repair.json"
    if not report_path.exists():
        return None
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None

    total = int(report.get("total_ticks") or 0)
    fps = float(report.get("master_fps") or report.get("tick_rate_fps") or 0.0)
    first_tick = report.get("first_tick")
    if total <= 0 or fps <= 0:
        return None

    cameras: dict[int, CameraTrack] = {}
    source_times: dict[int, np.ndarray] = {}
    for cam in report.get("cameras", []):
        if cam.get("skipped") or not cam.get("repaired_video"):
            continue
        idx = int(cam["index"])
        video = session / cam["repaired_video"]
        map_path = video.with_name(video.stem + ".repair_map.csv")
        if not video.exists() or not map_path.exists():
            continue
        missing = np.ones(total, dtype=bool)
        src_ids = np.full(total, -1, dtype=np.int64)
        with map_path.open(newline="") as f:
            for row in csv.DictReader(f):
                k = int(row["output_frame_index"])
                if 0 <= k < total:
                    flag = (row.get("missing") or row.get("duplicated") or "").strip().lower()
                    missing[k] = flag == "true"
                    src_ids[k] = int(row["source_frame_id"])
        cameras[idx] = CameraTrack(idx, video, np.arange(total, dtype=np.int64), missing)

        ts_path = session / "video" / f"timestamps_cam{idx}.csv"
        if ts_path.exists():
            ids, times = _read_timestamps(ts_path)
            lookup = dict(zip(ids.tolist(), times.tolist(), strict=False))
            source_times[idx] = np.array(
                [
                    lookup.get(int(s), -1) if not m else -1
                    for s, m in zip(src_ids, missing, strict=False)
                ],
                dtype=np.int64,
            )
    if not cameras:
        return None

    times = None
    if first_tick is not None:
        log = _read_tick_log(session / "video" / "action_ticks.csv")
        if log is not None and len(log) >= int(first_tick) + total:
            times = log[int(first_tick) : int(first_tick) + total]
    if times is None:
        times = _times_from_sources(source_times, total, fps)
    return Timeline(
        "synced", times, fps, cameras, int(first_tick) if first_tick is not None else None
    )


def _times_from_sources(source_times: dict, total: int, fps: float) -> np.ndarray:
    """Tick times from the cameras' own frame times: the median over cameras
    that have a real frame at the tick, filled at the nominal rate elsewhere."""
    step = int(round(1e9 / fps))
    times = np.full(total, -1, dtype=np.int64)
    if source_times:
        stack = np.vstack(list(source_times.values())).astype(np.float64)
        stack[stack < 0] = np.nan
        with np.errstate(all="ignore"):
            med = np.nanmedian(stack, axis=0)
        ok = np.isfinite(med)
        times[ok] = med[ok].astype(np.int64)
    known = np.flatnonzero(times >= 0)
    if known.size == 0:
        return np.arange(total, dtype=np.int64) * step
    # Fill gaps by stepping from the nearest earlier known tick (or back from
    # the first one), keeping the series increasing.
    for k in range(total):
        if times[k] < 0:
            prev = known[known < k]
            if prev.size:
                times[k] = times[prev[-1]] + (k - prev[-1]) * step
            else:
                times[k] = times[known[0]] - (known[0] - k) * step
    return times


def load_raw(session: Path) -> Timeline | None:
    """Timeline from raw ``video/`` and ``sync_manifest.json``."""
    path = session / "sync_manifest.json"
    if not path.exists():
        return None
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    total = int(manifest.get("total_ticks") or 0)
    step = int(manifest.get("step_ns") or 0)
    origin = int(manifest.get("t_origin_ns") or 0)
    fps = float(manifest.get("master_fps") or 0.0)
    if total <= 0 or step <= 0:
        return None
    if fps <= 0:
        fps = 1e9 / step
    times = origin + np.arange(total, dtype=np.int64) * step

    cameras: dict[int, CameraTrack] = {}
    for cam in manifest.get("cameras", []):
        idx = int(cam["index"])
        name = cam.get("video_file") or f"video_{idx}.mp4"
        video = session / name
        if not video.exists():
            video = session / "video" / Path(name).name
        ts_path = session / "video" / f"timestamps_cam{idx}.csv"
        ids_by_tick = manifest.get("ticks", {}).get(f"cam{idx}_frame_ids", [])
        if not video.exists() or not ts_path.exists() or len(ids_by_tick) < total:
            continue
        ids, elapsed = _read_timestamps(ts_path)
        ordinal = {int(fid): i for i, fid in enumerate(ids.tolist())}
        frame_index = np.full(total, -1, dtype=np.int64)
        missing = np.ones(total, dtype=bool)
        for k in range(total):
            fid = int(ids_by_tick[k])
            i = ordinal.get(fid)
            if i is None:
                continue
            frame_index[k] = i
            missing[k] = abs(int(elapsed[i]) - int(times[k])) > step // 2
        cameras[idx] = CameraTrack(idx, video, frame_index, missing)
    if not cameras:
        return None
    return Timeline("raw", times, fps, cameras)


def load_timeline(session: str | Path, prefer_synced: bool = True) -> Timeline | None:
    """The best timeline the session offers (see the module docstring)."""
    session = Path(session)
    if prefer_synced:
        synced = load_synced(session)
        if synced is not None:
            return synced
    return load_raw(session)
