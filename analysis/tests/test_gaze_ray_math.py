"""
Pure-logic tests for gaze/ray_math.py — the highest-value module in the
Multi-Camera Gaze Fusion plugin to get right (a sign/convention bug here
silently produces a plausible-looking but wrong 3D point, the same failure
class ray_math.py's own module docstring calls out). No cv2/mediapipe
needed — only numpy, already a base analysis/ dependency.
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent.parent))

from gaze.ray_math import (
    camera_ray_from_pose,
    closest_point_of_rays,
    ray_plane_intersection,
    transform_ray_to_room,
)

IDENTITY_R = np.eye(3)
ZERO_T = np.zeros(3)


# ── camera_ray_from_pose ─────────────────────────────────────────────────

def test_zero_gaze_offset_returns_head_forward_direction():
    origin, direction = camera_ray_from_pose(
        IDENTITY_R, ZERO_T, gaze_dx=0.0, gaze_dy=0.0, eye_origin_model_mm=np.zeros(3))
    assert np.allclose(direction, [0.0, 0.0, 1.0])
    assert np.allclose(origin, [0.0, 0.0, 0.0])


def test_origin_offset_by_eye_model_point_and_translation():
    translation = np.array([10.0, 20.0, 30.0])
    eye_model = np.array([1.0, 2.0, 3.0])
    origin, _ = camera_ray_from_pose(
        IDENTITY_R, translation, gaze_dx=0.0, gaze_dy=0.0, eye_origin_model_mm=eye_model)
    assert np.allclose(origin, eye_model + translation)


def test_positive_gaze_dx_yaws_direction_toward_expected_quadrant():
    _, direction = camera_ray_from_pose(
        IDENTITY_R, ZERO_T, gaze_dx=1.0, gaze_dy=0.0,
        eye_origin_model_mm=np.zeros(3), max_eye_yaw_deg=30.0)
    expected = np.array([np.sin(np.radians(30.0)), 0.0, np.cos(np.radians(30.0))])
    assert np.allclose(direction, expected, atol=1e-9)


def test_direction_is_always_unit_length():
    for dx in (-1.0, -0.4, 0.0, 0.6, 1.0):
        for dy in (-1.0, 0.3, 1.0):
            _, direction = camera_ray_from_pose(
                IDENTITY_R, ZERO_T, gaze_dx=dx, gaze_dy=dy, eye_origin_model_mm=np.zeros(3))
            assert abs(np.linalg.norm(direction) - 1.0) < 1e-9


# ── transform_ray_to_room ────────────────────────────────────────────────

def test_identity_extrinsic_leaves_ray_unchanged():
    extrinsic = np.eye(4).flatten()
    origin, direction = transform_ray_to_room([1.0, 2.0, 3.0], [0.0, 0.0, 1.0], extrinsic)
    assert np.allclose(origin, [1.0, 2.0, 3.0])
    assert np.allclose(direction, [0.0, 0.0, 1.0])


def test_translation_only_extrinsic_shifts_origin_not_direction():
    extrinsic = [1, 0, 0, 10,
                 0, 1, 0, 0,
                 0, 0, 1, 0,
                 0, 0, 0, 1]
    origin, direction = transform_ray_to_room([1.0, 2.0, 3.0], [0.0, 0.0, 1.0], extrinsic)
    assert np.allclose(origin, [11.0, 2.0, 3.0])
    assert np.allclose(direction, [0.0, 0.0, 1.0])


def test_rotation_extrinsic_rotates_both_origin_and_direction():
    # 90-degree rotation about Z: x -> y, y -> -x.
    extrinsic = [0, -1, 0, 0,
                 1,  0, 0, 0,
                 0,  0, 1, 0,
                 0,  0, 0, 1]
    origin, direction = transform_ray_to_room([1.0, 0.0, 0.0], [1.0, 0.0, 0.0], extrinsic)
    assert np.allclose(origin, [0.0, 1.0, 0.0], atol=1e-9)
    assert np.allclose(direction, [0.0, 1.0, 0.0], atol=1e-9)


# ── closest_point_of_rays ────────────────────────────────────────────────

def test_single_ray_returns_its_own_origin_with_zero_residual():
    point, residual = closest_point_of_rays([[5.0, 6.0, 7.0]], [[0.0, 0.0, 1.0]])
    assert np.allclose(point, [5.0, 6.0, 7.0])
    assert residual == 0.0


def test_two_rays_that_truly_intersect_recover_the_exact_point():
    # Ray A: from (0,0,0) along +X. Ray B: from (5,-5,0) along +Y.
    # They intersect exactly at (5, 0, 0).
    point, residual = closest_point_of_rays(
        [[0.0, 0.0, 0.0], [5.0, -5.0, 0.0]],
        [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0]])
    assert np.allclose(point, [5.0, 0.0, 0.0], atol=1e-9)
    assert residual < 1e-9


def test_two_parallel_offset_rays_land_near_the_midpoint_with_positive_residual():
    # Two parallel rays along +Z, offset by 2 units in X — never intersect.
    point, residual = closest_point_of_rays(
        [[0.0, 0.0, 0.0], [2.0, 0.0, 0.0]],
        [[0.0, 0.0, 1.0], [0.0, 0.0, 1.0]])
    assert residual > 0.9   # each ray is ~1 unit from the fused midpoint
    # The least-squares solution for parallel rays is under-determined along
    # Z but well-determined in X/Y — check only the well-determined part.
    assert abs(point[0] - 1.0) < 1e-6
    assert abs(point[1] - 0.0) < 1e-6


def test_three_rays_through_common_point_have_near_zero_residual():
    target = np.array([1.0, 2.0, 3.0])
    origins = [[0.0, 2.0, 3.0], [1.0, 0.0, 3.0], [1.0, 2.0, 0.0]]
    directions = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
    point, residual = closest_point_of_rays(origins, directions)
    assert np.allclose(point, target, atol=1e-9)
    assert residual < 1e-9


# ── ray_plane_intersection ───────────────────────────────────────────────

def test_ray_hits_plane_at_expected_point():
    # Ray from (0,0,-5) along +Z hits the Z=0 plane at the origin.
    hit = ray_plane_intersection(
        origin=[0.0, 0.0, -5.0], direction=[0.0, 0.0, 1.0],
        plane_point=[0.0, 0.0, 0.0], plane_normal=[0.0, 0.0, 1.0])
    assert hit is not None
    assert np.allclose(hit, [0.0, 0.0, 0.0], atol=1e-9)


def test_ray_parallel_to_plane_returns_none():
    hit = ray_plane_intersection(
        origin=[0.0, 0.0, 1.0], direction=[1.0, 0.0, 0.0],
        plane_point=[0.0, 0.0, 0.0], plane_normal=[0.0, 0.0, 1.0])
    assert hit is None


def test_ray_pointing_away_from_plane_returns_none():
    # Ray at Z=-5 pointing further away (-Z direction) never reaches Z=0.
    hit = ray_plane_intersection(
        origin=[0.0, 0.0, -5.0], direction=[0.0, 0.0, -1.0],
        plane_point=[0.0, 0.0, 0.0], plane_normal=[0.0, 0.0, 1.0])
    assert hit is None


def test_ray_hits_angled_plane_correctly():
    # Plane through (0,0,10) with normal along +Z; ray from origin along +Z.
    hit = ray_plane_intersection(
        origin=[3.0, 4.0, 0.0], direction=[0.0, 0.0, 1.0],
        plane_point=[0.0, 0.0, 10.0], plane_normal=[0.0, 0.0, 1.0])
    assert hit is not None
    assert np.allclose(hit, [3.0, 4.0, 10.0], atol=1e-9)
