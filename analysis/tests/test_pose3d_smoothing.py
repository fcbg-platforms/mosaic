"""
Pure-logic tests for pose3d/smoothing.py — the centered per-axis median
filter used for 3D Pose Reconstruction's optional "keypoints_room_smoothed"
output field.
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent.parent))

from pose3d.smoothing import smooth_track_positions


def test_window_of_one_returns_unchanged_copy():
    positions = [np.array([0.0, 0.0, 0.0]), np.array([1.0, 1.0, 1.0]), None]
    out = smooth_track_positions(positions, window=1)

    assert out is not positions
    assert out[0] is not None
    np.testing.assert_array_equal(out[0], positions[0])
    np.testing.assert_array_equal(out[1], positions[1])
    assert out[2] is None


def test_smooths_a_synthetic_jittery_but_trending_sequence():
    # A clean linear trend (0, 10, 20, ...) with one tick perturbed far off.
    trend = [np.array([float(i) * 10.0, 0.0, 0.0]) for i in range(9)]
    trend[4] = np.array([1000.0, 0.0, 0.0])  # a single outlier tick

    out = smooth_track_positions(trend, window=5)

    # The outlier tick's own median-filtered value should land close to the
    # real local trend (~40), nowhere near the raw outlier (1000).
    assert abs(out[4][0] - 40.0) < 30.0


def test_gap_is_preserved_as_none_and_does_not_corrupt_neighbours():
    positions = [
        np.array([0.0, 0.0, 0.0]),
        np.array([10.0, 0.0, 0.0]),
        None,  # untriangulated tick — a real gap, never fabricated
        np.array([30.0, 0.0, 0.0]),
        np.array([40.0, 0.0, 0.0]),
    ]
    out = smooth_track_positions(positions, window=3)

    assert out[2] is None
    # Neighbours of the gap are windowed over the VALID-tick subsequence
    # only, so the gap doesn't dilute/skew their smoothing.
    for i in (0, 1, 3, 4):
        assert out[i] is not None


def test_single_valid_entry_returns_itself_unchanged():
    positions = [None, np.array([5.0, 5.0, 5.0]), None]
    out = smooth_track_positions(positions, window=5)

    assert out[0] is None
    assert out[2] is None
    np.testing.assert_array_equal(out[1], positions[1])


def test_all_none_returns_all_none():
    positions = [None, None, None]
    out = smooth_track_positions(positions, window=3)
    assert out == [None, None, None]


def test_even_window_is_rounded_up_to_next_odd():
    # window=4 -> treated as 5; just confirm it runs and produces a
    # reasonable (not obviously-wrong) smoothed value, not a crash.
    positions = [np.array([float(i), 0.0, 0.0]) for i in range(6)]
    out = smooth_track_positions(positions, window=4)
    assert len(out) == 6
    assert all(p is not None for p in out)
