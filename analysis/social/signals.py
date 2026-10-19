"""Each person's signals for the social measures, read from what the other
analyses wrote for the camera facing them.

Times are the session's elapsed clock (milliseconds), which every camera's
analyses share, so two cameras' signals line up without further syncing.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np


def _read(path: Path) -> dict | None:
    try:
        d = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return d if isinstance(d, dict) else None


def _latest(folder: Path, pattern: str) -> Path | None:
    files = sorted(folder.glob(pattern), key=lambda p: p.stat().st_mtime)
    return files[-1] if files else None


def face_dynamics(session: Path, cam: int) -> dict | None:
    """Head speed per frame (deg/s, NaN without a face) and event onsets."""
    d = _read(session / "face_dynamics" / f"video_{cam}.face_dynamics.json")
    if d is None or d.get("schema") != "mosaic-face-dynamics-v1" or not d.get("frames"):
        return None
    t = np.array([f["timestamp_ms"] for f in d["frames"]], float)
    v = np.array(
        [
            f["head_speed"]
            if f.get("face_detected") and f.get("head_speed") is not None
            else np.nan
            for f in d["frames"]
        ],
        float,
    )
    onsets: dict = {}
    for e in d.get("events", []):
        onsets.setdefault(e["kind"], []).append(e["start_ms"])
    return {"t_ms": t, "head_speed": v, "onsets_ms": onsets}


def body_movement(session: Path, cam: int) -> dict | None:
    """How fast the main person's body moves: the mean speed of their visible
    keypoints between frames, in body heights (box heights) per second. The
    main person is the largest one in view (the one this camera faces)."""
    path = _latest(session / "pose", f"video_{cam}.*.pose.json")
    d = _read(path) if path else None
    if d is None or not d.get("frames"):
        return None
    t, v = [], []
    prev = None  # (t_ms, subject_id, keypoints, visible, height)
    for f in d["frames"]:
        ts = f.get("timestamp_ns")
        if ts is None:
            continue
        tm = ts / 1e6
        people = [p for p in f.get("subjects") or [] if p.get("bbox_xyxy")]
        if not people:
            prev = None
            t.append(tm)
            v.append(np.nan)
            continue
        p = max(
            people,
            key=lambda q: (q["bbox_xyxy"][2] - q["bbox_xyxy"][0])
            * (q["bbox_xyxy"][3] - q["bbox_xyxy"][1]),
        )
        vis = np.asarray(p.get("visibilities", []), float) > 0.5
        kp = np.asarray(p.get("keypoints", []), float).ravel()
        if kp.size != 2 * vis.size:
            prev = None  # a malformed detection: no speed across it
            t.append(tm)
            v.append(np.nan)
            continue
        kp = kp.reshape(-1, 2)
        height = max(1.0, p["bbox_xyxy"][3] - p["bbox_xyxy"][1])
        speed = np.nan
        if prev is not None and prev[1] == p.get("subject_id") and tm > prev[0]:
            both = vis & prev[3] if len(prev[3]) == len(vis) else np.zeros(0, bool)
            if both.sum() >= 4:
                d_px = np.linalg.norm(kp[both] - prev[2][both], axis=1).mean()
                speed = d_px / ((height + prev[4]) / 2.0) / ((tm - prev[0]) / 1000.0)
        prev = (tm, p.get("subject_id"), kp, vis, height)
        t.append(tm)
        v.append(speed)
    # Pose ran but no frame had a time: an empty signal (a note names the
    # camera), not "not run".
    return {"t_ms": np.array(t, float), "body_speed": np.array(v, float)}


def heart_rate(session: Path, cam: int) -> dict | None:
    """Each heart-rate window's rate at its centre; windows with a poor signal
    (the app's lowest quality tier) are left out."""
    path = _latest(session / "rppg", f"video_{cam}.*.rppg.json")
    d = _read(path) if path else None
    if d is None or not d.get("windows"):
        return None
    t, v = [], []
    for w in d["windows"]:
        bpm = w.get("smoothed_bpm") or w.get("bpm")
        snr, valid = w.get("snr_db"), w.get("valid_frame_fraction")
        poor = (valid is not None and valid < 0.6) or (snr is not None and snr < -5.0)
        t.append((w["start_ms"] + w["end_ms"]) / 2.0)
        v.append(np.nan if bpm is None or poor else bpm)
    return {"t_ms": np.array(t, float), "bpm": np.array(v, float)}


def on_grid(t_ms, values, t0_ms: float, n: int, fs: float, max_gap_s: float = 0.0) -> np.ndarray:
    """``values`` on a grid of ``n`` samples at ``fs`` from ``t0_ms``: the mean
    of the samples in each bin; with ``max_gap_s``, empty bins between samples
    at most that far apart are filled linearly (for sparse signals such as
    heart-rate windows). NaN where nothing was measured."""
    t = np.asarray(t_ms, float)
    v = np.asarray(values, float)
    ok = np.isfinite(t) & np.isfinite(v)
    t, v = t[ok], v[ok]
    out = np.full(n, np.nan)
    if t.size == 0:
        return out
    if max_gap_s > 0:
        grid = t0_ms + np.arange(n) * 1000.0 / fs
        order = np.argsort(t)
        t, v = t[order], v[order]
        out = np.interp(grid, t, v, left=np.nan, right=np.nan)
        k = np.searchsorted(t, grid)
        gap = np.full(n, np.inf)
        inside = (k > 0) & (k < t.size)
        gap[inside] = (t[k[inside]] - t[k[inside] - 1]) / 1000.0
        exact = np.isin(grid, t)
        out[(gap > max_gap_s) & ~exact] = np.nan
        return out
    idx = np.floor((t - t0_ms) * fs / 1000.0).astype(int)
    keep = (idx >= 0) & (idx < n)
    s = np.bincount(idx[keep], weights=v[keep], minlength=n)
    c = np.bincount(idx[keep], minlength=n)
    with np.errstate(invalid="ignore"):
        out = np.where(c > 0, s / np.maximum(c, 1), np.nan)
    return out
