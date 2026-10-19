"""Quality checks on a 3D reconstruction (pose3d/audit.py): jumps, swaps,
bone lengths, and how often each camera is dropped."""

from __future__ import annotations

import numpy as np
from pose3d.audit import identity_audit, view_agreement

FPS = 25.0
# A tiny body: four torso keypoints (0-3) around the centre, one bone 0-1.
TORSO = (0, 1, 2, 3)
EDGES = [(0, 1)]


def _person(track_id, centre, bone_mm=400.0):
    c = np.asarray(centre, dtype=float)
    kps = [
        (c + [-bone_mm / 2, 0, 0]).tolist(),
        (c + [bone_mm / 2, 0, 0]).tolist(),
        (c + [0, 300, 0]).tolist(),
        (c + [0, -300, 0]).tolist(),
    ]
    return {"track_id": track_id, "keypoints_room": kps, "keypoints_valid": [True] * 4}


def _frames(people_by_tick):
    return [
        {"tick": t, "timestamp_ns": int(t / FPS * 1e9), "people": people}
        for t, people in enumerate(people_by_tick)
    ]


def _audit(frames):
    return identity_audit(frames, EDGES, torso=TORSO)


def test_two_people_sitting_still_are_clean():
    frames = _frames([[_person(1, [0, 0, 2000]), _person(2, [1000, 0, 2000])]] * 50)
    a = _audit(frames)
    assert a["track_count"] == 2
    assert a["jumps"] == 0 and a["swaps"] == 0 and a["size_violations"] == 0
    assert a["short_tracks"] == 0
    assert a["tracks"]["1"]["median_bone_length_mm"]["0-1"] == 400.0
    assert a["tracks"]["1"]["bone_length_cv"] == 0.0


def test_a_name_swap_is_found():
    # Track 1 at x=0 and track 2 at x=1000 exchange labels at tick 20.
    ticks = []
    for t in range(40):
        a, b = (0, 1000) if t < 20 else (1000, 0)
        ticks.append([_person(1, [a, 0, 2000]), _person(2, [b, 0, 2000])])
    audit = _audit(_frames(ticks))
    assert audit["jumps"] == 2  # both tracks jump at the swap
    assert audit["swaps"] == 1


def test_a_single_jump_is_not_a_swap():
    ticks = [[_person(1, [0 if t < 10 else 600, 0, 2000])] for t in range(20)]
    audit = _audit(_frames(ticks))
    assert audit["jumps"] == 1
    assert audit["swaps"] == 0


def test_walking_is_not_a_jump():
    # 1.5 m/s: 60 mm per tick at 25 fps.
    ticks = [[_person(1, [60.0 * t, 0, 2000])] for t in range(50)]
    assert _audit(_frames(ticks))["jumps"] == 0


def test_a_bone_off_its_length_is_counted_and_short_tracks_too():
    ticks = [[_person(1, [0, 0, 2000], bone_mm=400.0 if t != 7 else 700.0)] for t in range(30)]
    ticks[0].append(_person(9, [3000, 0, 2000]))  # one tick only
    audit = _audit(_frames(ticks))
    assert audit["tracks"]["1"]["size_violations"] == 1
    assert audit["size_violations"] == 1
    assert audit["short_tracks"] == 1


def test_view_agreement_percentages():
    v = view_agreement({0: 90, 1: 100}, {0: 10, 2: 5})
    assert v["0"] == {"used": 90, "dropped": 10, "unresolved": 0, "dropped_pct": 10.0}
    assert v["1"]["dropped_pct"] == 0.0
    assert v["2"] == {"used": 0, "dropped": 5, "unresolved": 0, "dropped_pct": 100.0}


def test_hips_going_out_of_view_are_not_a_jump():
    # Seated behind a table: keypoint 3 (a "hip") seen every other tick. The
    # body is lopsided (keypoint 2 far up), so the mean of whatever is seen
    # would move 150 mm per tick (3.75 m/s) though nobody moves.
    ticks = []
    for t in range(30):
        p = _person(1, [0, 0, 2000])
        p["keypoints_room"][2] = [0.0, 900.0, 2000.0]
        if t % 2:
            p["keypoints_valid"] = [True, True, True, False]
            p["keypoints_room"][3] = None
        ticks.append([p])
    assert _audit(_frames(ticks))["jumps"] == 0


def test_view_agreement_counts_unresolved_keypoints():
    v = view_agreement({0: 10}, {}, {0: 4, 1: 4})
    assert v["0"]["unresolved"] == 4
    assert v["1"] == {"used": 0, "dropped": 0, "unresolved": 4, "dropped_pct": None}
