"""
People from 3D tracks, for the 3D Pose Reconstruction plugin
(analysis/run_pose3d.py).

The tracker (pose3d/tracker.py) ends a track when nobody is matched to it for
more than a few ticks, and a returning person starts a new one: one person who
walks out of most cameras' view for a moment becomes several tracks. On
sub-32_ses-04 (room 11, one person, mostly seen by two cameras) one person
was six tracks, four under a second.

Here tracks are joined into people. A track continues the person whose last
appearance ended shortly before it began, close enough to where it begins:
within ``MAX_GAP_S`` seconds, and no farther than ``MAX_JUMP_MM`` plus
walking speed (``WALK_MM_S``) over the gap. Of several such people the
nearest is taken. Tracks that overlap in time are never joined (two people
at once), so two people seen together stay two people.

Each person entry then carries ``person_id`` (1, 2, ...: "Person 1" in the
app) next to its ``track_id``, and the result lists the people with their
tracks and how long each was seen.
"""

from __future__ import annotations

import numpy as np

#: The longest gap a person can be missing and still continue, seconds.
MAX_GAP_S = 3.0
#: How far a person can be from where they were, plus walking over the gap.
MAX_JUMP_MM = 500.0
WALK_MM_S = 1500.0


def _centre(person: dict) -> np.ndarray | None:
    pts = [
        p
        for p, ok in zip(person["keypoints_room"], person["keypoints_valid"], strict=False)
        if ok and p is not None
    ]
    return np.mean(np.asarray(pts, dtype=float), axis=0) if pts else None


def join_tracks(results: list[dict]) -> list[dict]:
    """Adds ``person_id`` to every person entry of `results` (mutated in
    place) and returns the people: ``{"person_id", "track_ids", "seconds"}``,
    numbered by when they first appear."""
    tracks: dict[int, dict] = {}
    for entry in results:
        t = entry["timestamp_ns"] / 1e9
        for person in entry["people"]:
            c = _centre(person)
            tr = tracks.setdefault(
                person["track_id"],
                {"first": t, "last": t, "start": c, "end": c, "ticks": 0},
            )
            tr["last"] = t
            tr["ticks"] += 1
            if c is not None:
                tr["end"] = c
                if tr["start"] is None:
                    tr["start"] = c

    people: list[dict] = []
    person_of: dict[int, int] = {}
    for tid, tr in sorted(tracks.items(), key=lambda kv: (kv[1]["first"], kv[0])):
        best, best_d = None, None
        for pe in people:
            gap = tr["first"] - pe["last"]
            if gap <= 0 or gap > MAX_GAP_S or pe["end"] is None or tr["start"] is None:
                continue
            dist = float(np.linalg.norm(tr["start"] - pe["end"]))
            if dist <= MAX_JUMP_MM + WALK_MM_S * gap and (best_d is None or dist < best_d):
                best, best_d = pe, dist
        if best is None:
            best = {"person_id": len(people) + 1, "track_ids": [], "last": tr["last"], "ticks": 0}
            people.append(best)
        best["track_ids"].append(tid)
        best["last"] = tr["last"]
        best["end"] = tr["end"] if tr["end"] is not None else best.get("end")
        best["ticks"] += tr["ticks"]
        person_of[tid] = best["person_id"]

    for entry in results:
        for person in entry["people"]:
            person["person_id"] = person_of[person["track_id"]]

    step = _tick_step_s(results)
    return [
        {
            "person_id": pe["person_id"],
            "track_ids": pe["track_ids"],
            "seconds": round(pe["ticks"] * step, 2),
        }
        for pe in people
    ]


def _tick_step_s(results: list[dict]) -> float:
    """The time one analysed tick stands for, from the result's own times."""
    if len(results) < 2:
        return 0.0
    times = np.array([e["timestamp_ns"] for e in results], dtype=float) / 1e9
    steps = np.diff(times)
    steps = steps[steps > 0]
    return float(np.median(steps)) if len(steps) else 0.0
