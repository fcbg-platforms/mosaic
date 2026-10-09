"""
Tests for gaze/ray_math.py: the frame conventions every other gaze module
relies on. Numpy only.
"""

import math
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from gaze.ray_math import (
    angle_deg,
    head_direction_from_yaw_pitch,
    ray_plane_intersection,
    robust_mean_direction,
    rotation_matrix_from_rotvec,
    rotvec_from_rotation_matrix,
    transform_direction,
    transform_point,
    unit,
    yaw_pitch_from_head_direction,
)

# ── Rotations ────────────────────────────────────────────────────────────────


@pytest.mark.parametrize(
    "rotvec",
    [[0.0, 0.0, 0.0], [0.3, -0.2, 0.1], [0.0, math.pi / 2, 0.0], [1.0, 1.0, 1.0], [0, 0, 3.1]],
)
def test_rotvec_round_trip(rotvec):
    r = rotation_matrix_from_rotvec(rotvec)
    assert np.allclose(r @ r.T, np.eye(3), atol=1e-12)
    assert np.isclose(np.linalg.det(r), 1.0)
    assert np.allclose(rotation_matrix_from_rotvec(rotvec_from_rotation_matrix(r)), r, atol=1e-9)


def test_rotation_about_z_turns_x_into_y():
    r = rotation_matrix_from_rotvec([0.0, 0.0, math.pi / 2])
    assert np.allclose(r @ [1.0, 0.0, 0.0], [0.0, 1.0, 0.0])


def test_rigid_transforms_apply_rotation_then_translation():
    m = np.eye(4)
    m[:3, :3] = rotation_matrix_from_rotvec([0.0, 0.0, math.pi / 2])
    m[:3, 3] = [10.0, 0.0, 0.0]
    assert np.allclose(transform_point(m, [1.0, 0.0, 0.0]), [10.0, 1.0, 0.0])
    assert np.allclose(transform_direction(m.ravel(), [1.0, 0.0, 0.0]), [0.0, 1.0, 0.0])


# ── Gaze angles in the head frame ────────────────────────────────────────────


def test_straight_ahead_is_minus_z():
    assert np.allclose(head_direction_from_yaw_pitch(0.0, 0.0), [0.0, 0.0, -1.0])
    assert yaw_pitch_from_head_direction([0.0, 0.0, -1.0]) == pytest.approx((0.0, 0.0))


def test_positive_yaw_looks_towards_head_x_and_positive_pitch_looks_up():
    d = head_direction_from_yaw_pitch(math.radians(30), 0.0)
    assert d[0] > 0 and d[2] < 0
    d = head_direction_from_yaw_pitch(0.0, math.radians(20))
    assert d[1] < 0  # head +y is down


@pytest.mark.parametrize("yaw,pitch", [(0.4, -0.2), (-1.0, 0.5), (0.0, 1.2)])
def test_yaw_pitch_round_trip(yaw, pitch):
    d = head_direction_from_yaw_pitch(yaw, pitch)
    assert np.isclose(np.linalg.norm(d), 1.0)
    assert yaw_pitch_from_head_direction(d) == pytest.approx((yaw, pitch))


def test_angle_between_vectors():
    assert angle_deg([1, 0, 0], [0, 1, 0]) == pytest.approx(90.0)
    assert angle_deg([1, 0, 0], [2, 0, 0]) == pytest.approx(0.0, abs=1e-6)
    assert angle_deg([1, 0, 0], [-1, 0, 0]) == pytest.approx(180.0)


# ── Ray and plane ────────────────────────────────────────────────────────────


def test_ray_hits_plane_in_front():
    point, t = ray_plane_intersection([0, 0, 0], [0, 0, 2], [0, 0, 100], [0, 0, -1])
    assert np.allclose(point, [0, 0, 100]) and t == pytest.approx(100.0)


def test_ray_parallel_or_away_from_plane_misses():
    assert ray_plane_intersection([0, 0, 0], [1, 0, 0], [0, 0, 100], [0, 0, 1]) is None
    assert ray_plane_intersection([0, 0, 0], [0, 0, -1], [0, 0, 100], [0, 0, 1]) is None


# ── Robust direction mean ────────────────────────────────────────────────────


def test_robust_mean_ignores_one_outlier():
    good = [unit([0.02, 0.0, 1.0]), unit([-0.02, 0.01, 1.0]), unit([0.0, -0.01, 1.0])]
    outlier = unit([1.0, 0.0, 1.0])  # 45 degrees off
    mean, dispersion, kept = robust_mean_direction(good + [outlier], [1, 1, 1, 1])
    assert angle_deg(mean, [0, 0, 1]) < 1.0
    assert list(kept) == [True, True, True, False]
    assert dispersion < 2.0


def test_robust_mean_respects_weights_and_skips_zero_weight():
    mean, _, kept = robust_mean_direction([[0, 0, 1], unit([0.2, 0, 1])], [3.0, 1.0])
    assert 0.0 < angle_deg(mean, [0, 0, 1]) < angle_deg(mean, unit([0.2, 0, 1]))
    mean, _, kept = robust_mean_direction([[0, 0, 1], [1, 0, 0]], [1.0, 0.0])
    assert np.allclose(mean, [0, 0, 1]) and list(kept) == [True, False]


def test_robust_mean_with_no_usable_input_is_none():
    assert robust_mean_direction([[0, 0, 1]], [0.0]) is None
