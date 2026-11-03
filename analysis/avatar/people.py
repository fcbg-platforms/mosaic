"""The 3D Pose people an avatar is fitted to, and the plain geometry around
the fit (no body model needed, so it is tested on its own)."""

from __future__ import annotations

import numpy as np

#: COCO keypoints (3D Pose's order): the ones a body fit uses.
NOSE, L_SHOULDER, R_SHOULDER, L_HIP, R_HIP, L_ANKLE, R_ANKLE = 0, 5, 6, 11, 12, 15, 16


def load_people(skeleton3d: dict) -> dict:
    """``{person_id: {"ticks", "time_ms", "keypoints"}}`` from a 3D Pose
    result: per person (3D Pose's ``person_id``, several tracks of one
    person together), the ticks it was seen, their times and its 17
    smoothed keypoints there, in metres (NaN where not found). A person seen
    twice in one tick (two tracks) keeps the first."""
    out: dict = {}
    for f in skeleton3d.get("frames", []):
        for p in f.get("people", []):
            pid = p.get("person_id", p.get("track_id"))
            e = out.setdefault(pid, {"ticks": [], "time_ms": [], "keypoints": []})
            if e["ticks"] and e["ticks"][-1] == f["tick"]:
                continue
            pts = p.get("keypoints_room_smoothed") or p.get("keypoints_room") or []
            e["ticks"].append(f["tick"])
            e["time_ms"].append(f.get("timestamp_ns", 0) / 1e6)
            e["keypoints"].append(
                [[np.nan] * 3 if v is None else [c / 1000.0 for c in v] for v in pts]
            )
    for e in out.values():
        e["ticks"] = np.array(e["ticks"], int)
        e["time_ms"] = np.array(e["time_ms"], float)
        e["keypoints"] = np.array(e["keypoints"], float).reshape(len(e["ticks"]), -1, 3)
    return out


def up_frame(keypoints: np.ndarray) -> np.ndarray:
    """A rotation (rows) from the room into a frame with z up. Up is the
    mean direction from the ankles to the shoulders over the recording;
    from the hips when the ankles are never seen (everyone behind a desk);
    the room's -y (the reference camera's up) when neither is. x is the
    room's x made level, y completes a right-handed frame."""
    mid = lambda a, b: (keypoints[:, a] + keypoints[:, b]) / 2  # noqa: E731
    up = None
    for low in ((L_ANKLE, R_ANKLE), (L_HIP, R_HIP)):
        d = mid(L_SHOULDER, R_SHOULDER) - mid(*low)
        seen = np.isfinite(d).all(-1)
        if seen.any():
            up = d[seen].mean(axis=0)
            break
    if up is None or np.linalg.norm(up) < 1e-9:
        up = np.array([0.0, -1.0, 0.0])
    up = up / np.linalg.norm(up)
    x = np.array([1.0, 0.0, 0.0])
    if abs(x @ up) > 0.9:  # the room's x is nearly up: use its z instead
        x = np.array([0.0, 0.0, 1.0])
    x = x - (x @ up) * up
    x /= np.linalg.norm(x)
    return np.stack([x, np.cross(up, x), up])


def usable(valid: np.ndarray, min_joints: int = 8) -> np.ndarray:
    """Per tick (``valid``: N x 17 joints found), whether it can start a
    fit: at least ``min_joints`` joints, both hips and both shoulders among
    them (the hips say where the body is and which way it faces)."""
    return (
        (valid.sum(axis=1) >= min_joints)
        & valid[:, L_HIP]
        & valid[:, R_HIP]
        & valid[:, L_SHOULDER]
        & valid[:, R_SHOULDER]
    )


def nearest_sample(ticks: np.ndarray, sample: np.ndarray) -> np.ndarray:
    """Per tick, the index (into ``sample``, indices of ``ticks``, sorted) of
    the sampled tick nearest in time."""
    at = ticks[sample]
    k = (
        np.clip(np.searchsorted(at, ticks), 1, len(at) - 1)
        if len(at) > 1
        else np.zeros(len(ticks), int)
    )
    if len(at) > 1:
        k = np.where(np.abs(at[k - 1] - ticks) <= np.abs(at[k] - ticks), k - 1, k)
    return k


def smooth_mask(ticks: np.ndarray, max_gap: int = 2) -> np.ndarray:
    """Per inner sample (``len - 2``), whether its neighbours are at most
    ``max_gap`` ticks away on both sides: smoothness is only asked of a
    pose across a short gap, never across a person's absence."""
    d = np.diff(ticks)
    return (d[:-1] <= max_gap) & (d[1:] <= max_gap)


def chunks(n: int, size: int, overlap: int) -> list:
    """``[(start, end, keep_start, keep_end)]`` covering ``range(n)`` in
    pieces of ``size`` overlapping by ``overlap``; each piece keeps its
    middle, so the joins sit away from a piece's free ends."""
    if n <= size:
        return [(0, n, 0, n)]
    out, start = [], 0
    step = size - overlap
    while True:
        end = min(start + size, n)
        keep_start = 0 if start == 0 else start + overlap // 2
        keep_end = n if end == n else end - overlap // 2
        out.append((start, end, keep_start, keep_end))
        if end == n:
            return out
        start += step


def valid_bits(keypoints: np.ndarray) -> np.ndarray:
    """Per tick, a bit per keypoint (bit k: keypoint k was measured)."""
    seen = np.isfinite(keypoints).all(axis=-1)
    return (seen * (1 << np.arange(seen.shape[1]))).sum(axis=1).astype(np.uint32)
