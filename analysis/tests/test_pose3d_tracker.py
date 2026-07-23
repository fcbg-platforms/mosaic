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
    assert len(set(ids1)) == 2   # the far centroid did not reuse the existing track


def test_track_ends_after_max_gap_ticks_with_no_match():
    tracker = PersonTracker3D(max_gap_ticks=2, max_jump_mm=100.0)
    ids0 = tracker.update(0, [np.array([0.0, 0.0, 0.0])])
    original_id = ids0[0]

    tracker.update(1, [])   # gap tick 1
    tracker.update(2, [])   # gap tick 2 (== max_gap_ticks, still alive)
    tracker.update(3, [])   # gap tick 3 (> max_gap_ticks, track must be pruned)

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
    tracker.update(3, [])   # tick - last_tick == 3 == max_gap_ticks, not yet pruned
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
