"""
Pure-logic tests for pose3d/association.py — a bug here silently merges two
different people into one 3D reconstruction, or splits one real person into
two, so this is as high-value to get right as triangulation.py itself.
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent.parent))

from pose3d.association import PersonObservation, cluster_people, match_camera_pair
from pose3d.triangulation import CameraGeom, project_point_px

# 6 COCO-shaped keypoints is plenty for these tests (min_shared_keypoints=4 default).
_N_KP = 6


def _look_at_camera(position, target=(0.0, 0.0, 0.0), up=(0.0, 1.0, 0.0)):
    position = np.array(position, dtype=np.float64)
    target = np.array(target, dtype=np.float64)
    up = np.array(up, dtype=np.float64)

    forward = target - position
    forward = forward / np.linalg.norm(forward)
    right = np.cross(up, forward)
    right = right / np.linalg.norm(right)
    true_up = np.cross(forward, right)

    r = np.column_stack([right, true_up, forward])
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


def _observe(person_index, camera_index, cam, keypoints_room, visibility=1.0):
    pixels = np.array([project_point_px(kp, cam) for kp in keypoints_room])
    vis = np.full(len(keypoints_room), visibility)
    return PersonObservation(camera_index=camera_index, person_index=person_index,
                              keypoints_px=pixels, visibilities=vis)


def _cluster_keypoints(center, spread=80.0):
    """A small, fixed articulated-body-like cluster of 3D points around
    `center` — different centers give genuinely different "people"."""
    offsets = np.array([
        [0, 0, 0], [spread, 0, 0], [-spread, 0, 0],
        [0, spread, 0], [0, -spread, 0], [spread, spread, 0],
    ])[:_N_KP]
    return np.array(center) + offsets


def test_match_camera_pair_matches_the_same_person_across_two_cameras():
    cam_a = _make_camera(0, [800.0, 0.0, 0.0])
    cam_b = _make_camera(1, [-800.0, 400.0, 200.0])

    kps = _cluster_keypoints([0.0, 0.0, 1000.0])
    obs_a = [_observe(0, 0, cam_a, kps)]
    obs_b = [_observe(0, 1, cam_b, kps)]

    matches = match_camera_pair(obs_a, obs_b, cam_a, cam_b)
    assert matches == [(0, 0, matches[0][2])]
    assert matches[0][2] < 1.0   # near-zero reprojection cost for a true match


def test_match_camera_pair_rejects_a_mismatched_pair():
    # With only 2 cameras, ANY pair of detections has SOME 3D point that
    # (near-)explains both — two independent rays in 3D generically have a
    # well-defined closest point, so picking two unrelated real 3D people
    # can coincidentally still yield a low-cost "explanation" if their rays
    # happen to nearly cross. The unambiguous way to construct a genuinely
    # inconsistent pair (mirroring triangulate_with_rejection's own
    # bad-view test) is to take ONE real detection and directly corrupt the
    # other camera's observed pixels by a large, fixed offset — not to pick
    # a second, different-but-real 3D point.
    cam_a = _make_camera(0, [800.0, 0.0, 0.0])
    cam_b = _make_camera(1, [-800.0, 400.0, 200.0])

    kps = _cluster_keypoints([0.0, 0.0, 1000.0])
    obs_a = [_observe(0, 0, cam_a, kps)]
    obs_b_bad = _observe(0, 1, cam_b, kps)
    obs_b_bad.keypoints_px = obs_b_bad.keypoints_px + np.array([300.0, 300.0])

    matches = match_camera_pair(obs_a, [obs_b_bad], cam_a, cam_b, max_pair_cost_px=20.0)
    assert matches == []


def test_pairwise_cost_is_inf_below_min_shared_keypoints():
    cam_a = _make_camera(0, [800.0, 0.0, 0.0])
    cam_b = _make_camera(1, [-800.0, 400.0, 200.0])
    kps = _cluster_keypoints([0.0, 0.0, 1000.0])

    obs_a = _observe(0, 0, cam_a, kps)
    obs_b = _observe(0, 1, cam_b, kps, visibility=1.0)
    # Only 2 of 6 keypoints visible in B — below the default min of 4.
    obs_b.visibilities[2:] = 0.0

    from pose3d.association import pairwise_cost
    cost = pairwise_cost(obs_a, obs_b, cam_a, cam_b, min_shared_keypoints=4)
    assert cost == float("inf")


def test_cluster_people_separates_two_distinct_people_across_two_cameras():
    cam_a = _make_camera(0, [1500.0, 0.0, 0.0])
    cam_b = _make_camera(1, [-1500.0, 800.0, 300.0])

    person1 = _cluster_keypoints([-400.0, 0.0, 1000.0])
    person2 = _cluster_keypoints([400.0, 0.0, 1000.0])

    observations = {
        0: [_observe(0, 0, cam_a, person1), _observe(1, 0, cam_a, person2)],
        1: [_observe(0, 1, cam_b, person1), _observe(1, 1, cam_b, person2)],
    }
    cameras = {0: cam_a, 1: cam_b}

    clusters = cluster_people(observations, cameras)
    assert len(clusters) == 2
    for cluster in clusters:
        assert {c for c, _ in cluster} == {0, 1}

    # By construction, person_index 0 in camera 0 IS the same physical
    # person as person_index 0 in camera 1 (person1's real 3D keypoints
    # were used for both), and likewise for index 1 / person2 — a bug that
    # cross-wires the pairing (e.g. matches cam0's person 0 to cam1's
    # person 1) would fail this exact check.
    as_dicts = [dict(cluster) for cluster in clusters]
    assert {0: 0, 1: 0} in as_dicts
    assert {0: 1, 1: 1} in as_dicts


def test_cluster_people_drops_unmatched_single_camera_detection():
    cam_a = _make_camera(0, [1500.0, 0.0, 0.0])
    cam_b = _make_camera(1, [-1500.0, 800.0, 300.0])

    shared_person = _cluster_keypoints([0.0, 0.0, 1000.0])
    only_in_a = _cluster_keypoints([900.0, 900.0, 2500.0])

    observations = {
        0: [_observe(0, 0, cam_a, shared_person), _observe(1, 0, cam_a, only_in_a)],
        1: [_observe(0, 1, cam_b, shared_person)],
    }
    cameras = {0: cam_a, 1: cam_b}

    clusters = cluster_people(observations, cameras)
    assert len(clusters) == 1
    assert {c for c, _ in clusters[0]} == {0, 1}
