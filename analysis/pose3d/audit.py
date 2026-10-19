"""Quality checks on a 3D pose reconstruction: is each track one person, and
does each camera agree with the others?

Frame by frame, geometry cannot tell whether track 1 at one tick is the same
person as track 1 at the next. Physics can: a body does not move several
metres per second while seated, two people's tracks that both jump at once and
would not jump with their labels exchanged have swapped names, and a person's
bones do not change length. These counts follow ``temporal_audit`` in the 4by4k
pipeline (``fourk/assoc.py``, MIT licence), adapted to MOSAIC's tracks.
"""

from __future__ import annotations

import warnings

import numpy as np

# COCO-17 shoulders and hips: they move with the body, unlike an arm.
COCO_TORSO = (5, 6, 11, 12)


def _points(person: dict) -> np.ndarray:
    """(K, 3) keypoints, NaN where not valid."""
    kv, kr = person["keypoints_valid"], person["keypoints_room"]
    out = np.full((len(kv), 3), np.nan)
    for i, (p, v) in enumerate(zip(kr, kv, strict=False)):
        if v and p is not None:
            out[i] = p
    return out


def _move(a: np.ndarray, b: np.ndarray, torso: tuple) -> float | None:
    """How far a body moved between two observations: the mean distance of
    the torso keypoints valid in both (else of any keypoint valid in both).
    Only keypoints seen both times count, so hips going out of view behind
    a table do not move the body. None when no keypoint is shared."""
    n = min(len(a), len(b))
    both = np.isfinite(a[:n]).all(axis=1) & np.isfinite(b[:n]).all(axis=1)
    if not both.any():
        return None
    rows = [i for i in torso if i < n and both[i]]
    if not rows:
        rows = np.flatnonzero(both)
    return float(np.mean(np.linalg.norm(b[rows] - a[rows], axis=1)))


def identity_audit(
    frames: list,
    skeleton_edges=(),
    edge_names=None,
    torso=COCO_TORSO,
    jump_speed_mm_s: float = 3000.0,
    max_gap_s: float = 0.5,
    bone_tolerance: float = 0.25,
    short_track_s: float = 1.0,
) -> dict:
    """Physical implausibilities in a reconstruction's track identities.

    Parameters
    ----------
    frames : list of dict
        ``{"tick", "timestamp_ns", "people": [{"track_id", "keypoints_room",
        "keypoints_valid"}, ...]}``, in time order (``run_pose3d.py``'s frames).
    skeleton_edges : sequence of (int, int)
        Keypoint index pairs whose distance is a bone (``COCO_SKELETON``).
    edge_names : list of str, optional
        One name per edge for the output; default ``"a-b"``.
    torso : tuple of int
        Keypoints whose movement is the body's (those valid in both of two
        observations; any shared keypoint when no torso one is).
    jump_speed_mm_s : float, default 3000
        The body moving faster than this between two observations at
        most `max_gap_s` apart is a jump (a seated or walking person moves
        well under 2 m/s).
    max_gap_s : float, default 0.5
        Observations further apart are not compared.
    bone_tolerance : float, default 0.25
        A bone more than this fraction away from the track's median length
        is a size violation.
    short_track_s : float, default 1.0
        Tracks lasting less are counted as short (fragments).

    Returns
    -------
    dict
        ``tracks`` (per track: observed ticks, duration, jumps, size
        violations, median bone lengths and the median coefficient of
        variation of bone lengths) and session totals ``track_count``,
        ``short_tracks``, ``jumps``, ``swaps``, ``size_violations``.
    """
    edges = [tuple(e) for e in skeleton_edges]
    names = list(edge_names) if edge_names is not None else [f"{a}-{b}" for a, b in edges]

    # Per track: (frame position, time s, keypoints, bone lengths).
    per_track: dict = {}
    for fi, frame in enumerate(frames):
        t = float(frame["timestamp_ns"]) / 1e9
        for person in frame["people"]:
            pts = _points(person)
            if not np.isfinite(pts).all(axis=1).any():
                continue
            k = len(pts)
            bones = [
                float(np.linalg.norm(pts[a] - pts[b])) if a < k and b < k else np.nan
                for a, b in edges
            ]
            per_track.setdefault(person["track_id"], []).append((fi, t, pts, bones))

    tracks: dict = {}
    jump_at: dict = {}  # frame position -> {track: (keypoints before, keypoints now)}
    for tid, obs in per_track.items():
        jumps = 0
        for (_, ta, pa, _), (fb, tb, pb, _) in zip(obs[:-1], obs[1:], strict=False):
            dt = tb - ta
            if dt <= 0 or dt > max_gap_s:
                continue
            d = _move(pa, pb, torso)
            if d is not None and d / dt > jump_speed_mm_s:
                jumps += 1
                jump_at.setdefault(fb, {})[tid] = (pa, pb)

        lengths = np.array([o[3] for o in obs], dtype=float).reshape(len(obs), len(edges))
        median_len: dict = {}
        cvs = []
        violations = 0
        if len(edges):
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", RuntimeWarning)
                med = np.nanmedian(lengths, axis=0)
                std = np.nanstd(lengths, axis=0)
            for j, name in enumerate(names):
                if np.isfinite(med[j]) and med[j] > 0:
                    median_len[name] = round(float(med[j]), 1)
                    cvs.append(float(std[j] / med[j]))
            with np.errstate(invalid="ignore"):
                off = np.abs(lengths - med) > bone_tolerance * med
            violations = int(np.any(off & np.isfinite(lengths), axis=1).sum())

        duration = obs[-1][1] - obs[0][1]
        tracks[str(tid)] = {
            "observed_ticks": len(obs),
            "duration_s": round(duration, 3),
            "jumps": jumps,
            "size_violations": violations,
            "bone_length_cv": round(float(np.median(cvs)), 4) if cvs else None,
            "median_bone_length_mm": median_len,
        }

    # A swap: two tracks jump at the same tick, and exchanging their labels
    # would remove both jumps.
    swaps = 0
    for jumped in jump_at.values():
        ids = sorted(jumped)
        for i in range(len(ids)):
            for k in range(i + 1, len(ids)):
                pa, ca = jumped[ids[i]]
                pb, cb = jumped[ids[k]]
                moves = [_move(pa, ca, torso), _move(pb, cb, torso)]
                moves += [_move(pb, ca, torso), _move(pa, cb, torso)]
                if any(m is None for m in moves):
                    continue
                stay, cross = moves[0] + moves[1], moves[2] + moves[3]
                if cross < 0.5 * stay:
                    swaps += 1

    return {
        "track_count": len(tracks),
        "short_tracks": sum(1 for t in tracks.values() if t["duration_s"] < short_track_s),
        "jumps": sum(t["jumps"] for t in tracks.values()),
        "swaps": swaps,
        "size_violations": sum(t["size_violations"] for t in tracks.values()),
        "params": {
            "jump_speed_mm_s": jump_speed_mm_s,
            "max_gap_s": max_gap_s,
            "bone_tolerance": bone_tolerance,
            "short_track_s": short_track_s,
        },
        "tracks": tracks,
    }


def view_agreement(used: dict, dropped: dict, unresolved: dict | None = None) -> dict:
    """Per camera: keypoints it contributed to (``used``), keypoints it saw
    but was dropped from as disagreeing with the other cameras (``dropped``,
    ``triangulate_with_rejection``), and keypoints it saw that no two
    cameras agreed on, so none was triangulated (``unresolved``; with only
    two cameras, which one was wrong cannot be told). ``dropped_pct`` is
    dropped over used plus dropped. A camera dropped far more often than
    the rest, or with many unresolved keypoints, has usually moved or is
    miscalibrated."""
    unresolved = unresolved or {}
    out = {}
    for cam in sorted(set(used) | set(dropped) | set(unresolved)):
        u, d = int(used.get(cam, 0)), int(dropped.get(cam, 0))
        out[str(cam)] = {
            "used": u,
            "dropped": d,
            "unresolved": int(unresolved.get(cam, 0)),
            "dropped_pct": round(100.0 * d / (u + d), 2) if u + d else None,
        }
    return out
