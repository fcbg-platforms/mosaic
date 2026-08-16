"""
Pure-geometry multi-view triangulation for the 3D Pose Reconstruction plugin
(analysis/run_pose3d.py). Turns each camera's already-computed 2D COCO
keypoints (analysis/pose/keypoints.py, written by run_pose.py) into 3D
room-space points, using the room extrinsic calibration from item 19's
ChArUco room calibration (src/calibration/room_calibration_manager.hpp,
CalibrationData::extrinsicRt, reaching this script via session_meta.json).

Zero new ML dependencies — everything here is numpy/opencv-python linear
algebra, both already base analysis/ dependencies. Follows the same "pure,
cv2-free, directly-testable core function isolated from the one necessary
cv2 call" discipline as analysis/gaze/ray_math.py: the DLT solve itself
(triangulate_point_dlt) never touches cv2; only normalize_point()
(undistortion) and reproject_error_px() (real per-camera reprojection,
needed for an interpretable, distortion-aware quality metric) do.

Convention (matches CalibrationData::extrinsicRt / room_frame::Mat4 exactly
— see src/core/settings.hpp and src/calibration/room_frame_solver.hpp):
extrinsic_rt is a row-major 4x4 homogeneous transform, ROOM_FROM_CAMERA
(point_room = R @ point_camera + t), camera 0 always identity, translation
in millimetres.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

import cv2
import numpy as np


@dataclass
class CameraGeom:
    """One calibrated camera's geometry, as needed for triangulation.

    Attributes
    ----------
    index : int
        Camera index, matching the session's ``camN`` numbering.
    camera_matrix : numpy.ndarray
        Real (distorted-space) intrinsic matrix, reshaped to ``(3, 3)``.
    dist_coeffs : numpy.ndarray
        OpenCV distortion coefficients, reshaped to a flat vector.
    extrinsic_rt : numpy.ndarray
        Row-major rigid transform, reshaped to ``(4, 4)`` —
        room-from-camera (``point_room = R @ point_camera + t``), matching
        ``CalibrationData::extrinsicRt`` exactly (see
        :doc:`/math/room_calibration`).
    camera_from_room : numpy.ndarray
        Computed automatically as ``invert_rt(extrinsic_rt)`` — the inverse
        transform every projection in this module actually uses.
    """
    index: int
    camera_matrix: np.ndarray      # (3,3)
    dist_coeffs: np.ndarray        # (5,)
    extrinsic_rt: np.ndarray       # (4,4) or flat-16, row-major, room_from_camera
    camera_from_room: np.ndarray = field(init=False)   # (4,4), computed via invert_rt()

    def __post_init__(self) -> None:
        self.camera_matrix = np.asarray(self.camera_matrix, dtype=np.float64).reshape(3, 3)
        self.dist_coeffs = np.asarray(self.dist_coeffs, dtype=np.float64).reshape(-1)
        self.extrinsic_rt = np.asarray(self.extrinsic_rt, dtype=np.float64).reshape(4, 4)
        self.camera_from_room = invert_rt(self.extrinsic_rt)


def invert_rt(m: np.ndarray) -> np.ndarray:
    """Invert a rigid-body (rotation + translation) transform.

    Parameters
    ----------
    m : array_like
        A row-major rigid transform, as a ``(4, 4)`` matrix or a flat
        length-16 array — ``[R | t]`` with the bottom row implicitly
        ``[0, 0, 0, 1]``.

    Returns
    -------
    numpy.ndarray
        The inverse transform, shape ``(4, 4)``: ``[R^T | -R^T @ t]``.

    Notes
    -----
    Must stay mathematically identical to ``room_frame::invert()``
    (``src/calibration/room_frame_solver.cpp``) — Python cannot call the
    C++ function directly, so this is a small, deliberate reimplementation
    of the exact same formula. See :doc:`/math/pose3d_reconstruction` for
    the room-frame convention this assumes.
    """
    m = np.asarray(m, dtype=np.float64).reshape(4, 4)
    r = m[:3, :3]
    t = m[:3, 3]
    out = np.eye(4, dtype=np.float64)
    out[:3, :3] = r.T
    out[:3, 3] = -r.T @ t
    return out


def normalize_point(uv_px, cam: CameraGeom) -> np.ndarray:
    """Undistort a single pixel into ideal, K-free normalized coordinates.

    Parameters
    ----------
    uv_px : array_like
        A single pixel coordinate ``(u, v)``, in that camera's real
        distorted pixel space.
    cam : CameraGeom
        The camera whose intrinsics/distortion coefficients to undistort
        against.

    Returns
    -------
    numpy.ndarray
        Length-2 array, the ideal (undistorted, ``K``-free) normalized
        coordinate.

    Notes
    -----
    Uses :func:`cv2.undistortPoints` with no ``P=`` argument — ``K`` is
    deliberately left out of the result, since :func:`triangulate_point_dlt`
    only ever consumes already-normalized points and its projection
    matrices (see :func:`projection_matrix`) carry no ``K`` term either.
    """
    pts = np.asarray(uv_px, dtype=np.float64).reshape(1, 1, 2)
    undistorted = cv2.undistortPoints(pts, cam.camera_matrix, cam.dist_coeffs)
    return undistorted.reshape(2)


def projection_matrix(cam: CameraGeom) -> np.ndarray:
    """Build a camera's ``K``-free ``(3, 4)`` projection matrix.

    Parameters
    ----------
    cam : CameraGeom
        The camera to build a projection matrix for.

    Returns
    -------
    numpy.ndarray
        ``P = camera_from_room[:3, :4]``, shape ``(3, 4)`` — no ``K`` term,
        matching :func:`normalize_point`'s already-``K``-free output, so the
        two combine directly in :func:`triangulate_point_dlt`.
    """
    return cam.camera_from_room[:3, :4]


def triangulate_point_dlt(points_normalized, projection_matrices) -> Optional[np.ndarray]:
    """Classic linear multi-view DLT (Direct Linear Transform) triangulation.

    Parameters
    ----------
    points_normalized : sequence of array_like
        One ideal, ``K``-free normalized ``(u, v)`` point per contributing
        view (see :func:`normalize_point`), same order as
        `projection_matrices`.
    projection_matrices : sequence of array_like
        One ``(3, 4)`` ``K``-free projection matrix per contributing view
        (see :func:`projection_matrix`), same order as
        `points_normalized`.

    Returns
    -------
    numpy.ndarray or None
        The triangulated 3D point in room space (mm), shape ``(3,)``, or
        ``None`` if triangulation isn't possible (see Notes).

    Notes
    -----
    Pure numpy, zero cv2. Each view *i* contributes 2 homogeneous rows
    (``u_i * P_i[2,:] - P_i[0,:]`` and ``v_i * P_i[2,:] - P_i[1,:]``) to one
    ``(2N, 4)`` system; the 3D point is the right-singular-vector for the
    smallest singular value, dehomogenized. Requires at least 2 views.
    Returns ``None`` on too few views, a failed SVD, or a degenerate
    (near-zero homogeneous coordinate) solution — never a fabricated point.
    See :doc:`/math/pose3d_reconstruction` for the full derivation.
    """
    if len(points_normalized) < 2 or len(points_normalized) != len(projection_matrices):
        return None

    rows = []
    for (u, v), p in zip(points_normalized, projection_matrices):
        rows.append(u * p[2, :] - p[0, :])
        rows.append(v * p[2, :] - p[1, :])
    a = np.asarray(rows, dtype=np.float64)

    try:
        _, _, vt = np.linalg.svd(a)
    except np.linalg.LinAlgError:
        return None

    x = vt[-1]
    if abs(x[3]) < 1e-12:
        return None
    return x[:3] / x[3]


def project_point_px(point_room, cam: CameraGeom) -> np.ndarray:
    """Project a 3D room-space point into one camera's real pixel space.

    Parameters
    ----------
    point_room : array_like
        A 3D point in room space (mm), shape ``(3,)``.
    cam : CameraGeom
        The camera to project through — its **real** (distorted)
        intrinsics are used, not the K-free normalized-coordinate space
        the rest of this module operates in.

    Returns
    -------
    numpy.ndarray
        Length-2 array, the projected pixel coordinate.

    Notes
    -----
    The shared primitive behind both :func:`reproject_error_px`
    (triangulation quality) and ``run_pose3d.py``'s per-camera overlay
    precomputation (``Skeleton3DPerson::reprojectedPx``, consumed by
    :cpp:class:`mosaic::Skeleton3DResult` with zero calibration math on the
    C++ side).
    """
    r_wc = cam.camera_from_room[:3, :3]
    t_wc = cam.camera_from_room[:3, 3]
    rvec, _ = cv2.Rodrigues(r_wc)
    projected, _ = cv2.projectPoints(
        np.asarray(point_room, dtype=np.float64).reshape(1, 3),
        rvec, t_wc.reshape(3, 1), cam.camera_matrix, cam.dist_coeffs)
    return projected.reshape(2)


def reproject_error_px(point_room, cam: CameraGeom, pixel_observed) -> float:
    """Compute one view's reprojection error for a triangulated point.

    Parameters
    ----------
    point_room : array_like
        A triangulated 3D point in room space (mm), shape ``(3,)``.
    cam : CameraGeom
        The camera to reproject through.
    pixel_observed : array_like
        The originally observed 2D pixel coordinate this camera actually
        detected, to compare the reprojection against.

    Returns
    -------
    float
        Euclidean pixel distance between the reprojected point and
        `pixel_observed` — an interpretable, distortion-aware quality
        metric, consistent with ``RoomCalibrationManager``'s own
        extrinsic-solve reprojection-RMS report (see
        :doc:`/math/room_calibration`).
    """
    projected_px = project_point_px(point_room, cam)
    observed_px = np.asarray(pixel_observed, dtype=np.float64).reshape(2)
    return float(np.linalg.norm(projected_px - observed_px))


@dataclass
class TriangulationResult:
    """The outcome of a single :func:`triangulate_with_rejection` call.

    Attributes
    ----------
    point_room : numpy.ndarray
        The triangulated 3D point in room space (mm), shape ``(3,)``.
    used_views : list of int
        Camera indices that actually contributed to `point_room` — after
        visibility filtering and, if a re-triangulation happened, after
        outlier rejection too.
    per_view_error_px : dict of int to float
        Each entry in `used_views`' own reprojection error (px) against
        `point_room`, keyed by camera index.
    """
    point_room: np.ndarray
    used_views: list
    per_view_error_px: dict


def triangulate_with_rejection(observations, cameras,
                                max_reprojection_error_px: float = 15.0,
                                min_visibility: float = 0.1) -> Optional[TriangulationResult]:
    """Triangulate one keypoint across cameras, rejecting bad views once.

    Parameters
    ----------
    observations : dict of int to tuple
        ``{cam_idx: (pixel_uv, visibility)}`` — one observation per camera
        that detected this keypoint at all, regardless of confidence.
    cameras : dict of int to CameraGeom
        Every calibrated camera available for this session, keyed by index.
    max_reprojection_error_px : float, default 15.0
        A view whose reprojection error exceeds this (px) after the first
        triangulation pass is dropped before a single re-triangulation.
    min_visibility : float, default 0.1
        Observations below this visibility/confidence are excluded before
        triangulation even starts.

    Returns
    -------
    TriangulationResult or None
        ``None`` if fewer than 2 views survive visibility filtering, the
        initial triangulation fails, or fewer than 2 views survive outlier
        rejection — never a fabricated point.

    Notes
    -----
    Filters by `min_visibility`, requires ``>=2`` remaining cameras,
    triangulates via :func:`normalize_point` + :func:`triangulate_point_dlt`,
    then drops any view whose real reprojection error exceeds
    `max_reprojection_error_px` and re-triangulates **once** from the
    remainder — no iterative chase, matching ``pose_kinematics.cpp``'s
    "skip, don't fabricate" discipline (see :doc:`/math/pose_kinematics`).
    """
    visible = {idx: uv for idx, (uv, vis) in observations.items()
               if vis >= min_visibility and idx in cameras}
    if len(visible) < 2:
        return None

    def _triangulate(view_ids):
        norm_pts = [normalize_point(visible[i], cameras[i]) for i in view_ids]
        p_mats = [projection_matrix(cameras[i]) for i in view_ids]
        return triangulate_point_dlt(norm_pts, p_mats)

    view_ids = sorted(visible.keys())
    point = _triangulate(view_ids)
    if point is None:
        return None

    errors = {i: reproject_error_px(point, cameras[i], visible[i]) for i in view_ids}
    kept = [i for i in view_ids if errors[i] <= max_reprojection_error_px]

    if len(kept) == len(view_ids):
        return TriangulationResult(point_room=point, used_views=kept,
                                    per_view_error_px={i: errors[i] for i in kept})

    if len(kept) < 2:
        return None

    point2 = _triangulate(kept)
    if point2 is None:
        return None
    errors2 = {i: reproject_error_px(point2, cameras[i], visible[i]) for i in kept}
    return TriangulationResult(point_room=point2, used_views=kept, per_view_error_px=errors2)
