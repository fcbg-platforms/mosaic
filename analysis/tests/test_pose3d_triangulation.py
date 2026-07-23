"""
Pure-logic tests for pose3d/triangulation.py — the highest-value module in
the 3D Pose Reconstruction plugin to get right, same failure class as
gaze/ray_math.py's own tests (a sign/row-vs-column-major bug here silently
produces a plausible-looking but wrong 3D point).
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent.parent))

from pose3d.triangulation import (
    CameraGeom,
    invert_rt,
    project_point_px,
    projection_matrix,
    triangulate_point_dlt,
    triangulate_with_rejection,
)


def _rt_translation_only(tx, ty, tz):
    m = np.eye(4)
    m[:3, 3] = [tx, ty, tz]
    return m


def _look_at_camera(position, target=(0.0, 0.0, 0.0), up=(0.0, 1.0, 0.0)):
    """Builds a room_from_camera extrinsic (camera at `position`, local +Z
    axis pointing toward `target`) for synthetic test fixtures — matches
    OpenCV's forward-is-+Z convention. Must produce a PROPER rotation
    (det=+1, not a reflection) — cv2.Rodrigues() silently produces garbage
    for a det=-1 matrix, which a naive cross(forward, up)/cross(right,
    forward) construction can accidentally yield depending on which side
    `target` is offset to. right = cross(up, forward) then
    true_up = cross(forward, right) guarantees right x true_up == forward
    (right-handed, det=+1) for any forward/up pair."""
    position = np.array(position, dtype=np.float64)
    target = np.array(target, dtype=np.float64)
    up = np.array(up, dtype=np.float64)

    forward = target - position
    forward = forward / np.linalg.norm(forward)
    right = np.cross(up, forward)
    right = right / np.linalg.norm(right)
    true_up = np.cross(forward, right)

    r = np.column_stack([right, true_up, forward])
    assert np.isclose(np.linalg.det(r), 1.0), "camera basis must be a proper rotation"
    m = np.eye(4)
    m[:3, :3] = r
    m[:3, 3] = position
    return m


def _make_camera(index, position, target=(0.0, 0.0, 0.0)):
    return CameraGeom(
        index=index,
        camera_matrix=np.eye(3),
        dist_coeffs=np.zeros(5),
        extrinsic_rt=_look_at_camera(position, target),
    )


# ── invert_rt ────────────────────────────────────────────────────────────

def test_invert_rt_translation_only():
    m = _rt_translation_only(10.0, 20.0, 30.0)
    inv = invert_rt(m)
    assert np.allclose(inv[:3, 3], [-10.0, -20.0, -30.0])
    assert np.allclose(inv[:3, :3], np.eye(3))


def test_invert_rt_composes_to_identity_for_arbitrary_transform():
    m = _look_at_camera([1000.0, 500.0, -2000.0], target=[0.0, 0.0, 0.0])
    inv = invert_rt(m)
    assert np.allclose(m @ inv, np.eye(4), atol=1e-9)
    assert np.allclose(inv @ m, np.eye(4), atol=1e-9)


# ── triangulate_point_dlt ────────────────────────────────────────────────

def test_triangulate_point_dlt_recovers_known_point_from_two_views():
    point = np.array([100.0, 50.0, 200.0])
    cam_a = _make_camera(0, [1500.0, 300.0, 0.0])
    cam_b = _make_camera(1, [-1200.0, 800.0, 500.0])

    def _project_normalized(cam):
        p_cam = cam.camera_from_room[:3, :3] @ point + cam.camera_from_room[:3, 3]
        return np.array([p_cam[0] / p_cam[2], p_cam[1] / p_cam[2]])

    pts = [_project_normalized(cam_a), _project_normalized(cam_b)]
    p_mats = [projection_matrix(cam_a), projection_matrix(cam_b)]

    recovered = triangulate_point_dlt(pts, p_mats)
    assert recovered is not None
    assert np.allclose(recovered, point, atol=1e-6)


def test_triangulate_point_dlt_recovers_known_point_from_four_views():
    point = np.array([-40.0, 300.0, 900.0])
    cams = [
        _make_camera(0, [1500.0, 0.0, 0.0]),
        _make_camera(1, [-1500.0, 0.0, 500.0]),
        _make_camera(2, [0.0, 1500.0, -300.0]),
        _make_camera(3, [0.0, -1200.0, 800.0]),
    ]

    def _project_normalized(cam):
        p_cam = cam.camera_from_room[:3, :3] @ point + cam.camera_from_room[:3, 3]
        return np.array([p_cam[0] / p_cam[2], p_cam[1] / p_cam[2]])

    pts = [_project_normalized(c) for c in cams]
    p_mats = [projection_matrix(c) for c in cams]

    recovered = triangulate_point_dlt(pts, p_mats)
    assert recovered is not None
    assert np.allclose(recovered, point, atol=1e-6)


def test_triangulate_point_dlt_requires_at_least_two_views():
    cam_a = _make_camera(0, [1500.0, 0.0, 0.0])
    assert triangulate_point_dlt([np.array([0.0, 0.0])], [projection_matrix(cam_a)]) is None


def test_triangulate_point_dlt_rejects_mismatched_input_lengths():
    cam_a = _make_camera(0, [1500.0, 0.0, 0.0])
    cam_b = _make_camera(1, [-1500.0, 0.0, 0.0])
    pts = [np.array([0.0, 0.0])]
    p_mats = [projection_matrix(cam_a), projection_matrix(cam_b)]
    assert triangulate_point_dlt(pts, p_mats) is None


# ── triangulate_with_rejection (exercises the real cv2 undistort/project
#    round-trip, not just the pure DLT core) ───────────────────────────────

def test_triangulate_with_rejection_recovers_point_from_three_clean_views():
    point = np.array([0.0, 0.0, 1000.0])
    cameras = {
        0: _make_camera(0, [800.0, 0.0, 0.0]),
        1: _make_camera(1, [-800.0, 400.0, 200.0]),
        2: _make_camera(2, [0.0, -900.0, -300.0]),
    }

    observations = {i: (project_point_px(point, cam), 1.0) for i, cam in cameras.items()}
    result = triangulate_with_rejection(observations, cameras, max_reprojection_error_px=5.0)

    assert result is not None
    assert np.allclose(result.point_room, point, atol=1e-2)
    assert set(result.used_views) == {0, 1, 2}
    assert all(e < 1.0 for e in result.per_view_error_px.values())


def test_triangulate_with_rejection_drops_a_bad_view_and_still_recovers_point():
    point = np.array([50.0, -50.0, 1200.0])
    cameras = {
        0: _make_camera(0, [800.0, 0.0, 0.0]),
        1: _make_camera(1, [-800.0, 400.0, 200.0]),
        2: _make_camera(2, [0.0, -900.0, -300.0]),
    }

    good_a = project_point_px(point, cameras[0])
    good_b = project_point_px(point, cameras[1])
    bad_c = project_point_px(point, cameras[2]) + np.array([500.0, 500.0])

    observations = {0: (good_a, 1.0), 1: (good_b, 1.0), 2: (bad_c, 1.0)}
    result = triangulate_with_rejection(observations, cameras, max_reprojection_error_px=15.0)

    assert result is not None
    assert 2 not in result.used_views
    assert set(result.used_views) == {0, 1}
    # After rejection, the final triangulation uses only cams 0/1's
    # ORIGINAL, un-perturbed observations — should recover the exact
    # point regardless of how far the bad view pulled the intermediate
    # (rejected) solve.
    assert np.allclose(result.point_room, point, atol=1e-2)


def test_triangulate_with_rejection_returns_none_below_min_visibility():
    point = np.array([0.0, 0.0, 1000.0])
    cameras = {
        0: _make_camera(0, [800.0, 0.0, 0.0]),
        1: _make_camera(1, [-800.0, 400.0, 200.0]),
    }
    observations = {
        0: (project_point_px(point, cameras[0]), 0.01),
        1: (project_point_px(point, cameras[1]), 0.01),
    }
    assert triangulate_with_rejection(observations, cameras, min_visibility=0.1) is None


def test_triangulate_with_rejection_returns_none_with_only_one_view():
    point = np.array([0.0, 0.0, 1000.0])
    cameras = {0: _make_camera(0, [800.0, 0.0, 0.0])}
    observations = {0: (project_point_px(point, cameras[0]), 1.0)}
    assert triangulate_with_rejection(observations, cameras) is None


def test_triangulate_with_rejection_returns_none_when_fewer_than_two_survive_rejection():
    point = np.array([0.0, 0.0, 1000.0])
    cameras = {
        0: _make_camera(0, [800.0, 0.0, 0.0]),
        1: _make_camera(1, [-800.0, 400.0, 200.0]),
    }
    good_a = project_point_px(point, cameras[0])
    bad_b = project_point_px(point, cameras[1]) + np.array([500.0, 500.0])
    observations = {0: (good_a, 1.0), 1: (bad_b, 1.0)}
    # Only 2 views total; rejecting one leaves just 1 — can't re-triangulate.
    assert triangulate_with_rejection(observations, cameras, max_reprojection_error_px=15.0) is None
