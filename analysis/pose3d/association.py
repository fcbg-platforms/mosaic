"""
Cross-camera person association for the 3D Pose Reconstruction plugin
(analysis/run_pose3d.py) — item 19 (Multi-Camera Gaze Fusion) never needed
this since it hard-codes "subject 0" only; multi-person sessions need a real
answer to "which detection in camera A is the same physical person as which
detection in camera B" before per-keypoint triangulation makes sense at all.

Deliberately reuses triangulation.py's 2-view DLT + real reprojection error
as the pairwise association COST, rather than a separate classical
epipolar-line-distance/fundamental-matrix module: for a calibrated (not
just relatively-related) room rig, "if these two detections were the same
rigid articulated body, how consistent is their 3D reconstruction" is a
strictly stronger, more physically meaningful signal than a per-keypoint
point-to-epipolar-line distance, and it reuses machinery this plugin
already needs for the real reconstruction step anyway — no separate
fundamental-matrix code path to keep in sync.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy.optimize import linear_sum_assignment

from .triangulation import (
    CameraGeom,
    normalize_point,
    projection_matrix,
    reproject_error_px,
    triangulate_point_dlt,
)


@dataclass
class PersonObservation:
    camera_index: int
    person_index: int  # per-frame YOLO enumeration index from .pose.json — NOT a track id
    keypoints_px: np.ndarray  # (K,2)
    visibilities: np.ndarray  # (K,)


def pairwise_cost(
    a: PersonObservation,
    b: PersonObservation,
    cam_a: CameraGeom,
    cam_b: CameraGeom,
    min_shared_keypoints: int = 4,
    min_visibility: float = 0.1,
) -> float:
    """Average 2-view reprojection error (px) across every keypoint index
    visible (>=min_visibility) in BOTH a and b. inf if fewer than
    min_shared_keypoints such indices exist, or if 2-view triangulation
    fails for a majority of them (a near-parallel/degenerate configuration —
    not evidence of a real match either way, so treated as "can't assess",
    not "definitely different people")."""
    n = min(len(a.visibilities), len(b.visibilities))
    shared = [
        i
        for i in range(n)
        if a.visibilities[i] >= min_visibility and b.visibilities[i] >= min_visibility
    ]
    if len(shared) < min_shared_keypoints:
        return float("inf")

    errors = []
    failures = 0
    for i in shared:
        norm_pts = [
            normalize_point(a.keypoints_px[i], cam_a),
            normalize_point(b.keypoints_px[i], cam_b),
        ]
        p_mats = [projection_matrix(cam_a), projection_matrix(cam_b)]
        point = triangulate_point_dlt(norm_pts, p_mats)
        if point is None:
            failures += 1
            continue
        e_a = reproject_error_px(point, cam_a, a.keypoints_px[i])
        e_b = reproject_error_px(point, cam_b, b.keypoints_px[i])
        errors.append((e_a + e_b) / 2.0)

    if not errors or failures > len(shared) / 2:
        return float("inf")
    return float(np.mean(errors))


def match_camera_pair(
    obs_a,
    obs_b,
    cam_a: CameraGeom,
    cam_b: CameraGeom,
    max_pair_cost_px: float = 20.0,
    min_shared_keypoints: int = 4,
):
    """Hungarian-assigns obs_a<->obs_b by pairwise_cost(), then discards any
    assigned pair whose cost exceeds max_pair_cost_px — Hungarian minimizes
    TOTAL cost but doesn't itself refuse an individually-bad pair when one
    side has more/fewer detections than the other, so this threshold is the
    actual "not obviously the same person" gate. Returns
    (person_index_a, person_index_b, cost) triples for accepted pairs only."""
    if not obs_a or not obs_b:
        return []

    cost_matrix = np.full((len(obs_a), len(obs_b)), float("inf"))
    for i, a in enumerate(obs_a):
        for j, b in enumerate(obs_b):
            cost_matrix[i, j] = pairwise_cost(a, b, cam_a, cam_b, min_shared_keypoints)

    # linear_sum_assignment can't handle a fully-inf row/column (no finite
    # option at all) — substitute a large-but-finite sentinel so the solver
    # still runs; the max_pair_cost_px filter below rejects any assignment
    # that only "won" because everything else was worse, not because it was
    # actually a good match.
    finite = cost_matrix[np.isfinite(cost_matrix)]
    sentinel = (
        (float(finite.max()) + max_pair_cost_px + 1.0) if finite.size else (max_pair_cost_px + 1.0)
    )
    safe_matrix = np.where(np.isfinite(cost_matrix), cost_matrix, sentinel)

    row_idx, col_idx = linear_sum_assignment(safe_matrix)

    matches = []
    for r, c in zip(row_idx, col_idx, strict=False):
        cost = cost_matrix[r, c]
        if np.isfinite(cost) and cost <= max_pair_cost_px:
            matches.append((int(r), int(c), float(cost)))
    return matches


class _UnionFind:
    def __init__(self) -> None:
        self._parent: dict = {}

    def find(self, x):
        self._parent.setdefault(x, x)
        while self._parent[x] != x:
            self._parent[x] = self._parent[self._parent[x]]
            x = self._parent[x]
        return x

    def union(self, a, b) -> None:
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self._parent[ra] = rb


def cluster_people(
    observations, cameras, max_pair_cost_px: float = 20.0, min_shared_keypoints: int = 4
):
    """observations: {camera_index: [PersonObservation, ...]}. Runs
    match_camera_pair() over EVERY camera pair (not just adjacent ones — a
    room-scale rig may have non-adjacent pairs with better mutual
    visibility than adjacent ones), unions accepted matches via union-find
    over (camera_index, person_index) nodes, and returns one cluster (list
    of (camera_index, person_index)) per connected component spanning
    >=2 distinct cameras. Singleton (1-camera, unmatched) clusters are
    dropped — a single view can't be triangulated.

    Known limitation, not fixed here: any single accepted pairwise match
    unions its two nodes regardless of how many OTHER camera pairs agree —
    with only 2 cameras total, two genuinely different people can (rarely)
    still pass max_pair_cost_px if their rays happen to nearly cross by
    coincidence (2 independent rays in 3D always have SOME closest point;
    reprojection error alone can't rule out a coincidental near-intersection
    the way a 3rd independent view's disagreement could). Rooms with 3+
    calibrated cameras are far less exposed to this, since a false-positive
    2-camera pairing only survives into the union if no other accepted pair
    disagrees — but it is not structurally impossible even then. A cost
    threshold tuned tighter than the default (max_pair_cost_px) is the
    practical mitigation; a full multi-pair consistency vote is future work."""
    uf = _UnionFind()
    nodes: set = set()
    cam_indices = sorted(observations.keys())

    for idx in cam_indices:
        for p in range(len(observations[idx])):
            nodes.add((idx, p))
            uf.find((idx, p))

    for a_i in range(len(cam_indices)):
        for b_i in range(a_i + 1, len(cam_indices)):
            cam_a_idx, cam_b_idx = cam_indices[a_i], cam_indices[b_i]
            if cam_a_idx not in cameras or cam_b_idx not in cameras:
                continue
            matches = match_camera_pair(
                observations[cam_a_idx],
                observations[cam_b_idx],
                cameras[cam_a_idx],
                cameras[cam_b_idx],
                max_pair_cost_px,
                min_shared_keypoints,
            )
            for pi, pj, _cost in matches:
                uf.union((cam_a_idx, pi), (cam_b_idx, pj))

    groups: dict = {}
    for node in nodes:
        groups.setdefault(uf.find(node), []).append(node)

    return [sorted(members) for members in groups.values() if len({cam for cam, _ in members}) >= 2]
