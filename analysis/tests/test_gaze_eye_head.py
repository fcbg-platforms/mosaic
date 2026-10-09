"""
Tests for the geometric gaze core: gaze/eye_model.py and gaze/head_pose.py,
plus the canonical face read from the MediaPipe bundle (skipped when no
face_landmarker.task has been downloaded yet).

Everything is checked against synthetic ground truth: a head at a known pose,
an eye rotated by a known angle, projected through a known camera.
"""

import glob
import math
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from gaze import eye_model
from gaze.eye_model import (
    IRIS_SPHERE_RADIUS_MM,
    estimate_eye,
    eye_openness,
    iris_on_sphere,
    visual_axis,
)
from gaze.head_pose import (
    CameraView,
    HeadPose,
    camera_pose_to_room,
    project_points,
    refine_joint,
    solve_pnp,
)
from gaze.ray_math import (
    angle_deg,
    head_direction_from_yaw_pitch,
    rotation_matrix_from_rotvec,
    unit,
    yaw_pitch_from_head_direction,
)

K = np.array([[1300.0, 0.0, 960.0], [0.0, 1300.0, 540.0], [0.0, 0.0, 1.0]])
DIST = np.array([-0.12, 0.08, 0.001, -0.0005, 0.0])

# ── Eye model ────────────────────────────────────────────────────────────────


def _eye_seen(yaw_deg, pitch_deg, centre=(0.0, 0.0, 2500.0), r_cam_head=None):
    """Camera ray to the iris of an eye at `centre` rotated by yaw/pitch
    (head frame), as a perfect detector would see it."""
    r = np.eye(3) if r_cam_head is None else np.asarray(r_cam_head)
    optical_head = head_direction_from_yaw_pitch(math.radians(yaw_deg), math.radians(pitch_deg))
    iris = np.asarray(centre) + IRIS_SPHERE_RADIUS_MM * (r @ optical_head)
    return iris / iris[2], optical_head


@pytest.mark.parametrize("yaw,pitch", [(0, 0), (15, 0), (-20, 10), (5, -15)])
def test_eye_rotation_is_recovered_through_the_iris_sphere(yaw, pitch):
    ray, optical_head = _eye_seen(yaw, pitch)
    est = estimate_eye(np.eye(3), [0.0, 0.0, 2500.0], ray, +1.0, 0.3, 4.0)
    expected = visual_axis(optical_head, +1.0)
    assert angle_deg(est.visual_axis_cam, expected) < 0.05


def test_head_rotation_alone_does_not_change_eye_in_head_angle():
    r = rotation_matrix_from_rotvec([0.0, math.radians(35), 0.0])  # head turned
    ray, optical_head = _eye_seen(10, 5, r_cam_head=r)
    est = estimate_eye(r, [0.0, 0.0, 2500.0], ray, -1.0, 0.3, 4.0)
    in_head = r.T @ est.visual_axis_cam
    assert angle_deg(in_head, visual_axis(optical_head, -1.0)) < 0.05


def test_kappa_turns_both_eyes_towards_the_nose_and_up():
    straight = np.array([0.0, 0.0, -1.0])
    right = visual_axis(straight, +1.0)  # nose towards head +x
    left = visual_axis(straight, -1.0)
    yaw_r, pitch_r = yaw_pitch_from_head_direction(right)
    yaw_l, _ = yaw_pitch_from_head_direction(left)
    assert yaw_r == pytest.approx(math.radians(eye_model.KAPPA_NASAL_DEG))
    assert yaw_l == pytest.approx(-math.radians(eye_model.KAPPA_NASAL_DEG))
    assert pitch_r == pytest.approx(math.radians(eye_model.KAPPA_UP_DEG))


def test_ray_missing_the_sphere_clamps_to_the_tangent():
    centre = np.array([0.0, 0.0, 2500.0])
    ray = np.array([50.0 / 2500.0, 0.0, 1.0])  # 50 mm off the centre, radius 9.5
    point, hit = iris_on_sphere(ray, centre)
    assert not hit
    assert np.linalg.norm(point - centre) == pytest.approx(IRIS_SPHERE_RADIUS_MM)


def test_implausible_eye_rotation_is_limited_and_distrusted():
    ray, _ = _eye_seen(60, 0)  # 60 degrees: beyond what eyes do
    est = estimate_eye(np.eye(3), [0.0, 0.0, 2500.0], ray, +1.0, 0.3, 4.0)
    plain = estimate_eye(np.eye(3), [0.0, 0.0, 2500.0], _eye_seen(20, 0)[0], +1.0, 0.3, 4.0)
    optical = visual_axis(head_direction_from_yaw_pitch(0.0, 0.0), +1.0)  # straight ahead + kappa
    assert angle_deg(est.visual_axis_cam, optical) <= eye_model.MAX_EYE_ROTATION_DEG + 6.0
    assert est.weight < 0.5 * plain.weight


def test_closed_eye_gets_no_weight_and_small_iris_gets_less():
    ray, _ = _eye_seen(0, 0)
    closed = estimate_eye(np.eye(3), [0, 0, 2500.0], ray, 1.0, 0.05, 4.0)
    tiny = estimate_eye(np.eye(3), [0, 0, 2500.0], ray, 1.0, 0.3, 2.0)
    clear = estimate_eye(np.eye(3), [0, 0, 2500.0], ray, 1.0, 0.3, 6.0)
    assert closed.weight == 0.0
    assert 0.0 < tiny.weight < clear.weight
    assert tiny.sigma_deg > clear.sigma_deg


def test_eye_turned_away_from_the_camera_gets_no_weight():
    r = rotation_matrix_from_rotvec([0.0, math.radians(100), 0.0])
    ray, _ = _eye_seen(0, 0, r_cam_head=r)
    assert estimate_eye(r, [0, 0, 2500.0], ray, 1.0, 0.3, 5.0).weight == 0.0


def test_eye_openness_ratio():
    assert eye_openness([0, 0], [10, 0], [5, -1.5], [5, 1.5]) == pytest.approx(0.3)


# ── Projection and head pose ─────────────────────────────────────────────────


def test_numpy_projection_matches_opencv():
    cv2 = pytest.importorskip("cv2")
    rng = np.random.default_rng(1)
    pts = rng.uniform([-600, -400, 1500], [600, 400, 3500], size=(50, 3))
    ours = project_points(pts, K, DIST)
    theirs, _ = cv2.projectPoints(pts, np.zeros(3), np.zeros(3), K, DIST)
    assert np.allclose(ours, theirs.reshape(-1, 2), atol=1e-6)


def _face_points(n=40, seed=3):
    """A rigid, face-sized, non-planar point set in the head frame."""
    rng = np.random.default_rng(seed)
    return rng.uniform([-70, -90, -80], [70, 90, -20], size=(n, 3))


def _camera(rotvec, position):
    """room_from_camera for a camera at `position` with rotation `rotvec`."""
    m = np.eye(4)
    m[:3, :3] = rotation_matrix_from_rotvec(rotvec)
    m[:3, 3] = position
    return m


def test_single_camera_pnp_recovers_the_head_pose():
    pytest.importorskip("cv2")
    model = _face_points()
    truth = HeadPose(
        rotation_matrix_from_rotvec([0.1, 0.4, -0.05]), np.array([80.0, -40.0, 2400.0]), 1.0, 0.0
    )
    pixels = project_points(truth.apply(model), K, DIST)
    pose = solve_pnp(model, pixels, K, DIST)
    assert pose is not None and pose.rms_px < 0.01
    assert np.allclose(pose.t, truth.t, atol=0.5)
    assert angle_deg(pose.r @ [0, 0, -1], truth.r @ [0, 0, -1]) < 0.05


def test_joint_fit_across_cameras_recovers_pose_and_face_scale():
    pytest.importorskip("scipy")
    model = _face_points()
    truth = HeadPose(
        rotation_matrix_from_rotvec([0.05, 0.2, 0.0]), np.array([100.0, 50.0, 2500.0]), 1.07, 0.0
    )
    cams = [_camera([0, 0, 0], [0, 0, 0]), _camera([0, -0.6, 0], [1500, 0, 300])]
    rng = np.random.default_rng(7)
    views = []
    for room_from_cam in cams:
        cam_from_room = np.linalg.inv(room_from_cam)
        pts_cam = truth.apply(model) @ cam_from_room[:3, :3].T + cam_from_room[:3, 3]
        px = project_points(pts_cam, K, DIST) + rng.normal(0, 0.5, (len(model), 2))
        views.append(CameraView(cam_from_room, K, DIST, model, px, np.ones(len(model))))
    # Start from a deliberately wrong pose and the canonical scale.
    start = HeadPose(
        rotation_matrix_from_rotvec([0.0, 0.3, 0.1]), np.array([60.0, 0.0, 2300.0]), 1.0, 0.0
    )
    pose = refine_joint(views, start)
    assert pose.scale == pytest.approx(1.07, abs=0.01)
    assert np.linalg.norm(pose.t - truth.t) < 10.0
    assert angle_deg(pose.r @ [0, 0, -1], truth.r @ [0, 0, -1]) < 0.5
    assert pose.rms_px < 1.0


def test_camera_pose_maps_into_the_room():
    pose_cam = HeadPose(np.eye(3), np.array([0.0, 0.0, 1000.0]), 1.0, 0.0)
    room = camera_pose_to_room(pose_cam, _camera([0, 0, 0], [500, 0, 0]))
    assert np.allclose(room.t, [500, 0, 1000])


# ── Canonical face (needs a downloaded face_landmarker.task) ─────────────────


def _task_file():
    root = Path(__file__).parent.parent
    found = glob.glob(str(root / "*" / "models" / "face_landmarker.task"))
    return found[0] if found else None


@pytest.mark.skipif(_task_file() is None, reason="no face_landmarker.task downloaded")
def test_canonical_face_is_metric_and_in_the_head_frame():
    from gaze.canonical import load_canonical_face

    face = load_canonical_face(_task_file())
    v = face.vertices_mm
    assert v.shape == (468, 3)
    eye_span = np.linalg.norm(v[33] - v[263])
    assert 80.0 < eye_span < 100.0  # outer eye corners, mm
    assert v[1][2] < v[33][2]  # nose tip is in front of the eyes (face looks -z)
    assert v[152][1] > v[10][1]  # chin below forehead (+y is down)
    assert v[263][0] > 0 > v[33][0]  # subject's left eye on +x
    assert len(face.rigid_ids) >= 6 and face.rigid_weights.max() == pytest.approx(1.0)
    assert unit(v[263] - v[33])[0] > 0.9
