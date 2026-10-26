"""Hand kinematics from the Hands points: positions, speeds, grip aperture,
hand opening and finger bend, per hand over time. See docs/math/hands.rst.

The points are in pixels on one camera. Speeds are also given in **hand
lengths per second**: divided by the hand's own palm length (wrist to the
middle finger's knuckle) over the second around, which cancels how far the
hand is from the camera. Grip aperture and hand opening are in palm lengths
too, the usual way to compare people with different hand sizes. Millimetres
come with 3D hands.
"""

from __future__ import annotations

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

WRIST, THUMB_TIP, INDEX_TIP, MIDDLE_MCP = 0, 4, 8, 9
TIPS = (4, 8, 12, 16, 20)
#: Each finger's points from its base to its tip (thumb from CMC).
FINGERS = {
    "thumb": (1, 2, 3, 4),
    "index": (5, 6, 7, 8),
    "middle": (9, 10, 11, 12),
    "ring": (13, 14, 15, 16),
    "pinky": (17, 18, 19, 20),
}
#: A point the model scored below this is not used.
MIN_POINT_SCORE = 0.2
#: Gaps up to this long are filled in a straight line; longer ones end a run.
#: At every Nth frame the step itself grows: see :func:`max_gap_s`.
MAX_FILL_S = 0.2
#: Fewer analysed frames per second than this are too few for speeds: a
#: reach is half a second.
MIN_RATE_HZ = 10.0
#: Runs shorter than this are left out: too short to filter.
MIN_RUN_S = 0.5
#: Low-pass cut-off: hand movements stay under it, the points' jitter above.
CUTOFF_HZ = 6.0
#: A frame whose palm is outside this share of the palm over the second
#: around is the wrong hand, or a badly placed one: left out.
PALM_RANGE = (0.67, 1.5)

COLUMNS = (
    "time_ms",
    "subject_id",
    "side",
    "wrist_x_px",
    "wrist_y_px",
    "palm_px",
    "wrist_speed_hl_s",
    "wrist_speed_px_s",
    "index_tip_speed_hl_s",
    "thumb_tip_speed_hl_s",
    "wrist_acceleration_hl_s2",
    "grip_aperture_palm",
    "hand_opening_palm",
    *(f"{f}_bend" for f in FINGERS),
)


def _running(values: np.ndarray, half: int, fn) -> np.ndarray:
    """``fn`` (a nan-aware reduction) over each sample's window of
    ``2 * half + 1``, edges padded with NaN."""
    padded = np.pad(values, (half, half), constant_values=np.nan)
    windows = sliding_window_view(padded, 2 * half + 1)
    out = np.full(len(values), np.nan)
    has = np.isfinite(windows).any(axis=1)
    if has.any():
        out[has] = fn(windows[has], axis=1)
    return out


def max_gap_s(rate: float) -> float:
    """The longest gap filled: :data:`MAX_FILL_S`, or two and a half
    analysed frames when that is longer (every Nth frame)."""
    return max(MAX_FILL_S, 2.5 / rate)


def clean(points: np.ndarray, scores: np.ndarray, fps: float) -> np.ndarray:
    """The points (N x 21 x 2, one run without long gaps) with the doubtful
    ones set to NaN: scored under :data:`MIN_POINT_SCORE`; every point of a
    frame whose palm length, where it can be measured, is off
    (:data:`PALM_RANGE`); and a point far from its own 7-frame median (a
    Hampel filter: over 3 scaled median absolute deviations and half a
    palm), where the model jumped to the wrong place for a frame or two."""
    xy = np.array(points, float)
    xy[np.asarray(scores) < MIN_POINT_SCORE] = np.nan
    palm = np.linalg.norm(xy[:, MIDDLE_MCP] - xy[:, WRIST], axis=1)
    around = _running(palm, max(1, int(fps / 2)), np.nanmedian)
    lo, hi = PALM_RANGE
    # A frame whose wrist or middle knuckle is unsure keeps its other points.
    off = np.isfinite(palm) & ~((palm >= lo * around) & (palm <= hi * around))
    xy[off] = np.nan
    for k in range(xy.shape[1]):
        for d in range(2):
            v = xy[:, k, d]
            med = _running(v, 3, np.nanmedian)
            mad = 1.4826 * _running(np.abs(v - med), 3, np.nanmedian)
            far = np.abs(v - med) > np.maximum(3.0 * mad, 0.5 * around)
            v[far] = np.nan
    return xy


def _runs(times_s: np.ndarray, max_gap_s: float) -> list:
    """``[(start, end)]`` index ranges with no gap over ``max_gap_s``."""
    if len(times_s) == 0:
        return []
    breaks = np.flatnonzero(np.diff(times_s) > max_gap_s + 1e-9) + 1
    return list(zip(np.r_[0, breaks], np.r_[breaks, len(times_s)], strict=True))


def resample(
    times_s: np.ndarray, xy: np.ndarray, grid: np.ndarray, max_gap: float = MAX_FILL_S
) -> np.ndarray:
    """Each point on the regular ``grid``: at a sample, or filled in a
    straight line between two of its samples at most ``max_gap`` apart; NaN
    where it was not seen, never held past its last sample."""
    out = np.full((len(grid), *xy.shape[1:]), np.nan)
    for k in range(xy.shape[1]):
        for d in range(xy.shape[2]):
            ok = np.isfinite(xy[:, k, d])
            if ok.sum() < 2:
                continue
            tk, vk = times_s[ok], xy[ok, k, d]
            j = np.clip(np.searchsorted(tk, grid), 1, len(tk) - 1)
            within = (grid >= tk[0] - 1e-9) & (grid <= tk[-1] + 1e-9)
            short = tk[j] - tk[j - 1] <= max_gap + 1e-9
            at_sample = (np.abs(tk[j] - grid) < 1e-6) | (np.abs(tk[j - 1] - grid) < 1e-6)
            keep = within & (short | at_sample)
            out[keep, k, d] = np.interp(grid[keep], tk, vk)
    return out


def lowpass(xy: np.ndarray, fps: float) -> np.ndarray:
    """A 2nd-order Butterworth low-pass run forwards and back (no delay),
    on each point's own unbroken stretches; a stretch too short to filter
    is dropped."""
    from scipy.signal import butter, filtfilt

    b, a = butter(2, min(CUTOFF_HZ, 0.4 * fps) / (fps / 2.0))
    pad = 3 * max(len(a), len(b))
    out = xy.copy()
    for k in range(xy.shape[1]):
        for d in range(xy.shape[2]):
            v = out[:, k, d]
            edges = np.flatnonzero(np.diff(np.r_[0, np.isfinite(v).astype(int), 0]))
            for r0, r1 in zip(edges[::2], edges[1::2], strict=True):
                v[r0:r1] = filtfilt(b, a, v[r0:r1]) if r1 - r0 > pad else np.nan
    return out


def measures(xy: np.ndarray, fps: float) -> dict:
    """Per sample, from filtered points on a regular grid at ``fps``."""
    dt = 1.0 / fps
    palm = np.linalg.norm(xy[:, MIDDLE_MCP] - xy[:, WRIST], axis=1)
    palm = _running(palm, max(1, int(fps / 2)), np.nanmedian)
    vel = np.gradient(xy, dt, axis=0) if len(xy) > 1 else np.full_like(xy, np.nan)
    speed = np.linalg.norm(vel, axis=2)
    acc = np.linalg.norm(np.gradient(vel, dt, axis=0), axis=2) if len(xy) > 1 else speed * np.nan
    tips = np.array([np.linalg.norm(xy[:, k] - xy[:, WRIST], axis=1) for k in TIPS])
    seen = np.isfinite(tips)
    opening = np.full(len(xy), np.nan)
    enough = seen.sum(axis=0) >= 3
    if enough.any():
        opening[enough] = np.nanmean(tips[:, enough], axis=0)
    out = {
        "wrist_x_px": xy[:, WRIST, 0],
        "wrist_y_px": xy[:, WRIST, 1],
        "palm_px": palm,
        "wrist_speed_hl_s": speed[:, WRIST] / palm,
        "wrist_speed_px_s": speed[:, WRIST],
        "index_tip_speed_hl_s": speed[:, INDEX_TIP] / palm,
        "thumb_tip_speed_hl_s": speed[:, THUMB_TIP] / palm,
        "wrist_acceleration_hl_s2": acc[:, WRIST] / palm,
        "grip_aperture_palm": np.linalg.norm(xy[:, THUMB_TIP] - xy[:, INDEX_TIP], axis=1) / palm,
        "hand_opening_palm": opening / palm,
    }
    # Bend: 0 for a straight finger, towards 1 curled. Straight stays
    # straight in any view (a projection keeps points on a line), so 0 is
    # reliable; how bent a bent finger looks depends on the view.
    for name, chain in FINGERS.items():
        seg = sum(
            np.linalg.norm(xy[:, b] - xy[:, a], axis=1)
            for a, b in zip(chain[:-1], chain[1:], strict=True)
        )
        reach = np.linalg.norm(xy[:, chain[-1]] - xy[:, chain[0]], axis=1)
        out[f"{name}_bend"] = 1.0 - reach / seg
    return out


def hand_series(samples: list, fps: float) -> dict | None:
    """One hand's measures on a regular grid at ``fps`` (the analysed
    frames' rate): ``samples`` is ``[(time_ms, points 21 x 2, scores 21)]``
    in time order. Runs split at gaps over :func:`max_gap_s`, each cleaned
    on its own; runs under :data:`MIN_RUN_S` left out. None when nothing
    could be measured, or under :data:`MIN_RATE_HZ`."""
    if not samples or fps < MIN_RATE_HZ:
        return None
    t = np.array([s[0] for s in samples], float) / 1000.0
    points = np.array([s[1] for s in samples], float)
    scores = np.array([s[2] for s in samples], float)
    gap = max_gap_s(fps)
    parts = []
    for a, b in _runs(t, gap):
        if t[b - 1] - t[a] < MIN_RUN_S:
            continue
        xy = clean(points[a:b], scores[a:b], fps)
        grid = np.arange(t[a], t[b - 1] + 1e-9, 1.0 / fps)
        m = measures(lowpass(resample(t[a:b], xy, grid, gap), fps), fps)
        m["time_ms"] = grid * 1000.0
        parts.append(m)
    if not parts:
        return None
    out = {k: np.concatenate([p[k] for p in parts]) for k in parts[0]}
    return out if np.isfinite(out["wrist_speed_px_s"]).any() else None


def summary(series: dict, fps: float) -> dict:
    """A hand's numbers over the recording: how long it was measured, how
    far the wrist went, its speeds, the grip aperture and hand opening."""

    def stat(key, fn, digits=2):
        v = series[key][np.isfinite(series[key])]
        return round(float(fn(v)), digits) if len(v) else None

    speed = series["wrist_speed_hl_s"]
    fine = np.isfinite(speed)
    return {
        "measured_s": round(float(fine.sum()) / fps, 1),
        "wrist_path_hl": round(float(np.sum(speed[fine]) / fps), 1),
        "wrist_speed_median_hl_s": stat("wrist_speed_hl_s", np.median),
        "wrist_speed_p95_hl_s": stat("wrist_speed_hl_s", lambda v: np.percentile(v, 95)),
        "grip_aperture_median_palm": stat("grip_aperture_palm", np.median),
        "grip_aperture_p95_palm": stat("grip_aperture_palm", lambda v: np.percentile(v, 95)),
        "hand_opening_median_palm": stat("hand_opening_palm", np.median),
    }


def hand_samples(frames: list) -> dict:
    """``{(subject_id, side): [(time_ms, points, scores)]}`` from a Hands
    frames file. Hands of no known person or side are left out: two of them
    in a row need not be one hand."""
    out: dict = {}
    for f in frames:
        for h in f["hands"]:
            if h["side"] == "unknown" or h["subject_id"] is None:
                continue
            key = (h["subject_id"], h["side"])
            out.setdefault(key, []).append((f["timestamp_ms"], h["keypoints"], h["scores"]))
    return out
