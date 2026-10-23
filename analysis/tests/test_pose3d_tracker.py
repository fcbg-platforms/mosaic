"""
Pure-logic tests for pose3d/tracker.py — cross-frame 3D track identity.
No cv2/numpy-heavy geometry needed here (unlike triangulation/association),
just the greedy nearest-centroid assignment itself.
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent.parent))

from pose3d.tracker import PersonTracker3D


def test_single_track_persists_across_small_movements():
    tracker = PersonTracker3D(max_gap_ticks=5, max_jump_mm=200.0)
    ids0 = tracker.update(0, [np.array([0.0, 0.0, 0.0])])
    ids1 = tracker.update(1, [np.array([10.0, 5.0, 0.0])])
    ids2 = tracker.update(2, [np.array([20.0, 10.0, 0.0])])
    assert ids0[0] == ids1[0] == ids2[0]


def test_new_centroid_far_from_existing_track_gets_a_new_id():
    tracker = PersonTracker3D(max_gap_ticks=5, max_jump_mm=100.0)
    ids0 = tracker.update(0, [np.array([0.0, 0.0, 0.0])])
    ids1 = tracker.update(1, [np.array([0.0, 0.0, 0.0]), np.array([5000.0, 5000.0, 0.0])])
    assert ids0[0] in ids1
    assert len(set(ids1)) == 2  # the far centroid did not reuse the existing track


def test_track_ends_after_max_gap_ticks_with_no_match():
    tracker = PersonTracker3D(max_gap_ticks=2, max_jump_mm=100.0)
    ids0 = tracker.update(0, [np.array([0.0, 0.0, 0.0])])
    original_id = ids0[0]

    tracker.update(1, [])  # gap tick 1
    tracker.update(2, [])  # gap tick 2 (== max_gap_ticks, still alive)
    tracker.update(3, [])  # gap tick 3 (> max_gap_ticks, track must be pruned)

    # A centroid reappearing at the ORIGINAL position after the track was
    # pruned must get a brand-new id, not silently resurrect the old one.
    ids4 = tracker.update(4, [np.array([0.0, 0.0, 0.0])])
    assert ids4[0] != original_id


def test_track_survives_exactly_max_gap_ticks_of_absence():
    tracker = PersonTracker3D(max_gap_ticks=3, max_jump_mm=100.0)
    ids0 = tracker.update(0, [np.array([0.0, 0.0, 0.0])])
    original_id = ids0[0]

    tracker.update(1, [])
    tracker.update(2, [])
    tracker.update(3, [])  # tick - last_tick == 3 == max_gap_ticks, not yet pruned
    ids4 = tracker.update(4, [np.array([1.0, 1.0, 0.0])])
    assert ids4[0] == original_id


def test_two_close_tracks_resolve_via_nearest_first_greedy_assignment():
    tracker = PersonTracker3D(max_gap_ticks=5, max_jump_mm=500.0)
    ids0 = tracker.update(0, [np.array([0.0, 0.0, 0.0]), np.array([1000.0, 0.0, 0.0])])
    id_left, id_right = ids0

    # At tick 1, both centroids moved slightly toward each other but are
    # still clearly closer to their own original track than to the other's.
    ids1 = tracker.update(1, [np.array([50.0, 0.0, 0.0]), np.array([950.0, 0.0, 0.0])])
    assert ids1[0] == id_left
    assert ids1[1] == id_right


def test_two_new_centroids_in_one_tick_each_get_a_distinct_id():
    tracker = PersonTracker3D(max_gap_ticks=5, max_jump_mm=200.0)
    ids0 = tracker.update(0, [np.array([0.0, 0.0, 0.0]), np.array([2000.0, 0.0, 0.0])])
    assert len(set(ids0)) == 2


def test_empty_tick_returns_empty_list_and_does_not_crash():
    tracker = PersonTracker3D()
    assert tracker.update(0, []) == []


# ── Joining tracks into people (pose3d/people.py) ────────────────────────────


def _frames(spans):
    """Ticks at 10 per second; `spans` is (track_id, first tick, last tick,
    x of the first tick in mm, x speed in mm per tick)."""
    out = []
    for tick in range(0, 200):
        people = []
        for tid, a, b, x0, vx in spans:
            if a <= tick <= b:
                x = x0 + vx * (tick - a)
                people.append(
                    {
                        "track_id": tid,
                        "keypoints_room": [[x, 0.0, 3000.0], [x, 500.0, 3000.0]],
                        "keypoints_valid": [True, True],
                    }
                )
        out.append({"tick": tick, "timestamp_ns": tick * 100_000_000, "people": people})
    return out


def test_one_person_split_by_a_short_gap_is_one_person():
    from pose3d.people import join_tracks

    # Walks out of view for 1 s and comes back 0.4 m on: one person.
    frames = _frames([(0, 0, 49, 0.0, 10.0), (1, 60, 99, 900.0, 10.0)])
    people = join_tracks(frames)
    assert people == [{"person_id": 1, "track_ids": [0, 1], "seconds": 9.0}]
    assert {p["person_id"] for f in frames for p in f["people"]} == {1}


def test_two_people_at_once_stay_two():
    from pose3d.people import join_tracks

    frames = _frames([(0, 0, 99, 0.0, 0.0), (1, 20, 99, 300.0, 0.0)])
    assert [p["track_ids"] for p in join_tracks(frames)] == [[0], [1]]


def test_a_far_jump_or_a_long_gap_is_another_person():
    from pose3d.people import join_tracks

    # 3 m away after 0.5 s: too far to have walked.
    far = _frames([(0, 0, 49, 0.0, 0.0), (1, 55, 99, 3000.0, 0.0)])
    assert len(join_tracks(far)) == 2
    # Back at the same place, but 5 s later: not joined either.
    late = _frames([(0, 0, 49, 0.0, 0.0), (1, 100, 149, 0.0, 0.0)])
    assert len(join_tracks(late)) == 2
