"""
Temporal smoothing for one 3D-pose track's per-keypoint trajectory
(analysis/run_pose3d.py). Per-frame triangulation is otherwise independent
tick-to-tick, so real reconstruction noise shows up as visible jitter in an
otherwise-static skeleton — this smooths that out for display without
touching the raw triangulated values, which are always kept alongside the
smoothed ones (see run_pose3d.py's "keypoints_room_smoothed" output field).

Centered per-axis MEDIAN filter, not a mean — same choice already made for
rPPG's own per-hop BPM smoothing (analysis/rppg/hr_estimation.py::
median_smooth()), for the same reason: robust to an occasional single-tick
triangulation outlier that a mean filter would be more sensitive to.
"""

from __future__ import annotations

import numpy as np


def smooth_track_positions(
    positions: list[np.ndarray | None], window: int
) -> list[np.ndarray | None]:
    """Centered per-axis median filter over one track's one keypoint's
    per-tick 3D positions.

    Parameters
    ----------
    positions : list of numpy.ndarray or None
        One entry per tick, in tick order, for a single (track, keypoint)
        pair. ``None`` marks a tick where this keypoint wasn't triangulated
        (an untracked gap) — never fabricated and never treated as a
        neighbour when smoothing another tick.
    window : int
        Filter width, in *valid* ticks (not raw tick count — a run of gaps
        doesn't widen the effective window). ``<= 1`` disables smoothing
        (returned unchanged, as a new list — not the same object). Even
        values are silently rounded up to the next odd number, matching
        this project's established defensive convention for smoothing-window
        parity (item 16, pose kinematics; rPPG's own ``median_smooth()``).

    Returns
    -------
    list of numpy.ndarray or None
        Same length as `positions`. Each valid tick's output is the
        per-axis median of the centered window of *valid* ticks around it
        (windowed over the valid-tick subsequence, so gaps don't dilute or
        skew a neighbouring window) — a single valid tick with no valid
        neighbours returns unchanged. Every ``None`` position stays
        ``None``.
    """
    if window <= 1:
        return list(positions)
    if window % 2 == 0:
        window += 1

    valid_indices = [i for i, p in enumerate(positions) if p is not None]
    if not valid_indices:
        return list(positions)

    valid_positions = np.array([positions[i] for i in valid_indices], dtype=float)
    half = window // 2

    out: list[np.ndarray | None] = list(positions)
    for j, i in enumerate(valid_indices):
        lo, hi = max(0, j - half), min(len(valid_indices), j + half + 1)
        out[i] = np.median(valid_positions[lo:hi], axis=0)

    return out
