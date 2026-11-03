"""The 3D hands of a session (``hands/hands3d.frames.json``, from Hands with
two or more calibrated cameras), given to the 3D Pose people they belong to,
for the avatar's fingers. See docs/math/avatar.rst.

A 3D hand is kept as one hand over time but says nothing of whose it is in
3D Pose's numbering, so each hand goes to the person whose wrist on the same
side is nearest to its wrist over the frames both are seen in.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from avatar.people import L_WRIST, R_WRIST

#: A hand is a person's when its wrist is within this of theirs (metres)...
WRIST_WITHIN_M = 0.15
#: ...in at least this share of the frames both are seen in.
WRIST_SHARE = 0.5
#: A hand frame and a 3D Pose tick this far apart (ms) are the same moment.
SAME_MOMENT_MS = 20.0
SIDES = ("left", "right")


def load_hands(path: Path) -> dict:
    """``{hand: {"side", "time_ms" (T), "points" (T x 21 x 3, room metres,
    NaN where not found)}}``; empty when there is no file."""
    if not path.exists():
        return {}
    out: dict = {}
    for f in json.loads(path.read_text(encoding="utf-8")).get("frames", []):
        for h in f.get("hands", []):
            if h.get("side") not in SIDES:
                continue
            e = out.setdefault(h["hand"], {"side": h["side"], "time_ms": [], "points": []})
            e["time_ms"].append(float(f["timestamp_ms"]))
            e["points"].append(
                [[np.nan] * 3 if p is None else [c / 1000.0 for c in p] for p in h["points_mm"]]
            )
    for e in out.values():
        e["time_ms"] = np.array(e["time_ms"], float)
        e["points"] = np.array(e["points"], float).reshape(len(e["time_ms"]), 21, 3)
    return out


def nearest_ticks(times: np.ndarray, at: np.ndarray, within_ms: float) -> np.ndarray:
    """Per time in ``at``, the index of the nearest of ``times`` (sorted),
    or -1 when it is further than ``within_ms``."""
    if len(times) == 0:
        return np.full(len(at), -1)
    k = np.searchsorted(times, at).clip(0, len(times) - 1)
    prev = (k - 1).clip(0)
    k = np.where(np.abs(times[prev] - at) < np.abs(times[k] - at), prev, k)
    return np.where(np.abs(times[k] - at) <= within_ms, k, -1)


def assign(hands: dict, people: dict) -> dict:
    """``{person: {side: {"time_ms", "points"}}}``: each hand given to the
    person (``people`` as load_people gives them, room metres) whose wrist
    on its side is within WRIST_WITHIN_M of its wrist in WRIST_SHARE of the
    frames both are seen in, the most such frames winning. A person's hands
    on one side are put together in time, but for its frames away from the
    person's wrist (kept where the wrist was not found); where two are seen
    at once, the nearer to the person's wrist is kept."""
    found: dict = {}
    for e in hands.values():
        wrist = e["points"][:, 0]
        joint = L_WRIST if e["side"] == "left" else R_WRIST
        best = None
        for pid, person in people.items():
            k = nearest_ticks(person["time_ms"], e["time_ms"], SAME_MOMENT_MS)
            theirs = np.where(
                (k >= 0)[:, None], person["keypoints"][np.maximum(k, 0), joint], np.nan
            )
            d = np.linalg.norm(wrist - theirs, axis=1)
            both = np.isfinite(d)
            if not both.any():
                continue
            near = int((d[both] <= WRIST_WITHIN_M).sum())
            if near / both.sum() >= WRIST_SHARE and (best is None or near > best[0]):
                best = (near, pid, d)
        if best is not None:
            found.setdefault(best[1], {}).setdefault(e["side"], []).append((e, best[2]))
    out: dict = {}
    for pid, sides in found.items():
        for side, parts in sides.items():
            by_time: dict = {}
            for e, d in parts:
                for i, t in enumerate(e["time_ms"]):
                    if d[i] > WRIST_WITHIN_M:
                        continue  # this frame, away from the person's wrist
                    dist = d[i] if np.isfinite(d[i]) else np.inf
                    if t not in by_time or dist < by_time[t][0]:
                        by_time[t] = (dist, e["points"][i])
            times = np.array(sorted(by_time))
            out.setdefault(pid, {})[side] = {
                "time_ms": times,
                "points": np.array([by_time[t][1] for t in times]),
            }
    return out
