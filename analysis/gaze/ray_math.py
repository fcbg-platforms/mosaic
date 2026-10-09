"""
Pure geometry for the Multi-Camera Gaze Fusion plugin: rigid transforms,
gaze angles, ray/plane intersection and a robust mean of unit directions.

Numpy only (no cv2, no MediaPipe), so every function here is unit-tested
directly. A sign or frame mistake in this module produces a plausible but
wrong 3D point, so the conventions are fixed in one place:

* **Head frame** (OpenCV style, millimetres): +x towards the subject's left
  (image right when they face the camera), +y down, +z into the head. The
  face looks along **-z**.
* **Camera frame** (OpenCV): +x right, +y down, +z forward.
* **Room frame**: the reference camera's frame, millimetres.
  ``extrinsic_rt`` is room-from-camera, ``p_room = R @ p_cam + t``.
"""

from __future__ import annotations

import math

import numpy as np


def rotation_matrix_from_rotvec(rotvec) -> np.ndarray:
    """Rotation matrix from an axis-angle vector (Rodrigues' formula).

    Parameters
    ----------
    rotvec : array_like
        Rotation axis scaled by the angle in radians, shape ``(3,)``.

    Returns
    -------
    numpy.ndarray
        ``(3, 3)`` rotation matrix.
    """
    v = np.asarray(rotvec, dtype=np.float64).reshape(3)
    theta = float(np.linalg.norm(v))
    if theta < 1e-12:
        return np.eye(3)
    k = v / theta
    kx = np.array([[0.0, -k[2], k[1]], [k[2], 0.0, -k[0]], [-k[1], k[0], 0.0]])
    return np.eye(3) + math.sin(theta) * kx + (1.0 - math.cos(theta)) * (kx @ kx)


def rotvec_from_rotation_matrix(r) -> np.ndarray:
    """Axis-angle vector of a rotation matrix (inverse of
    :func:`rotation_matrix_from_rotvec`).

    Parameters
    ----------
    r : array_like
        ``(3, 3)`` rotation matrix.

    Returns
    -------
    numpy.ndarray
        Rotation axis scaled by the angle in radians, shape ``(3,)``.
    """
    r = np.asarray(r, dtype=np.float64).reshape(3, 3)
    cos_t = max(-1.0, min(1.0, (np.trace(r) - 1.0) / 2.0))
    theta = math.acos(cos_t)
    if theta < 1e-9:
        return np.zeros(3)
    if math.pi - theta < 1e-6:
        # Near 180 degrees the antisymmetric part vanishes: take the axis
        # from the symmetric part instead.
        m = (r + np.eye(3)) / 2.0
        axis = m[:, int(np.argmax(np.diag(m)))]
        axis = axis / np.linalg.norm(axis)
        return axis * theta
    w = np.array([r[2, 1] - r[1, 2], r[0, 2] - r[2, 0], r[1, 0] - r[0, 1]])
    return w / (2.0 * math.sin(theta)) * theta


def unit(v) -> np.ndarray:
    """``v`` scaled to unit length (returned unchanged when it is zero)."""
    v = np.asarray(v, dtype=np.float64)
    n = float(np.linalg.norm(v))
    return v / n if n > 1e-12 else v


def angle_deg(a, b) -> float:
    """Angle between two vectors in degrees, numerically safe near 0 and 180."""
    a, b = unit(a), unit(b)
    return math.degrees(math.atan2(np.linalg.norm(np.cross(a, b)), float(np.dot(a, b))))


def transform_point(rt, p) -> np.ndarray:
    """Apply a ``(4, 4)`` rigid transform (or flat length 16) to a point."""
    m = np.asarray(rt, dtype=np.float64).reshape(4, 4)
    return m[:3, :3] @ np.asarray(p, dtype=np.float64) + m[:3, 3]


def transform_direction(rt, d) -> np.ndarray:
    """Apply only the rotation part of a rigid transform to a direction."""
    m = np.asarray(rt, dtype=np.float64).reshape(4, 4)
    return m[:3, :3] @ np.asarray(d, dtype=np.float64)


def yaw_pitch_from_head_direction(d_head) -> tuple[float, float]:
    """Gaze angles of a direction given in the head frame.

    Parameters
    ----------
    d_head : array_like
        Direction in the head frame (the face looks along -z).

    Returns
    -------
    tuple of float
        ``(yaw, pitch)`` in radians. Yaw is positive towards head +x (the
        subject's left), pitch is positive upwards (head -y). ``(0, 0)`` is
        straight ahead.
    """
    d = unit(d_head)
    yaw = math.atan2(d[0], -d[2])
    pitch = math.atan2(-d[1], math.hypot(d[0], d[2]))
    return yaw, pitch


def head_direction_from_yaw_pitch(yaw: float, pitch: float) -> np.ndarray:
    """Inverse of :func:`yaw_pitch_from_head_direction`: a unit direction in
    the head frame."""
    return np.array(
        [
            math.cos(pitch) * math.sin(yaw),
            -math.sin(pitch),
            -math.cos(pitch) * math.cos(yaw),
        ]
    )


def ray_plane_intersection(origin, direction, plane_point, plane_normal):
    """Where a ray meets a plane.

    Parameters
    ----------
    origin, direction : array_like
        Ray start and direction, shape ``(3,)``.
    plane_point, plane_normal : array_like
        Any point on the plane and its normal, shape ``(3,)``.

    Returns
    -------
    tuple or None
        ``(point, t)`` with ``point = origin + t * unit(direction)``, or
        ``None`` when the ray is parallel to the plane or the plane lies
        behind the ray (``t <= 0``).
    """
    o = np.asarray(origin, dtype=np.float64)
    d = unit(direction)
    n = unit(plane_normal)
    denom = float(np.dot(n, d))
    if abs(denom) < 1e-9:
        return None
    t = float(np.dot(n, np.asarray(plane_point, dtype=np.float64) - o)) / denom
    if t <= 0.0:
        return None
    return o + t * d, t


def robust_mean_direction(
    directions, weights, reject_deg: float = 20.0, iterations: int = 3
) -> tuple[np.ndarray, float, np.ndarray] | None:
    """Weighted mean of unit directions that ignores outliers.

    Starts from the weighted mean, then repeatedly drops every direction
    more than ``reject_deg`` away from the current estimate and re-averages
    the rest. One camera that misread an eye cannot drag the result.

    Parameters
    ----------
    directions : array_like
        Shape ``(N, 3)``, need not be normalised.
    weights : array_like
        Shape ``(N,)``, non-negative. Zero-weight directions are ignored.
    reject_deg : float, default 20.0
        Angular distance beyond which a direction counts as an outlier.
    iterations : int, default 3
        Maximum reject-and-average rounds.

    Returns
    -------
    tuple or None
        ``(mean_direction, dispersion_deg, kept_mask)``: the unit mean, the
        weighted RMS angle of the kept directions around it, and which
        inputs were kept. ``None`` when no input has positive weight.
    """
    d = np.asarray(directions, dtype=np.float64).reshape(-1, 3)
    w = np.asarray(weights, dtype=np.float64).reshape(-1)
    norms = np.linalg.norm(d, axis=1)
    valid = (w > 0.0) & (norms > 1e-12)
    if not valid.any():
        return None
    d = np.where(norms[:, None] > 1e-12, d / np.maximum(norms, 1e-12)[:, None], d)

    kept = valid.copy()
    mean = unit((w[kept, None] * d[kept]).sum(axis=0))
    for _ in range(iterations):
        angles = np.degrees(np.arccos(np.clip(d @ mean, -1.0, 1.0)))
        new_kept = valid & (angles <= reject_deg)
        if not new_kept.any():
            # Everything disagrees with the mean: keep the single heaviest
            # direction rather than inventing a compromise between them.
            new_kept = np.zeros_like(valid)
            new_kept[int(np.argmax(np.where(valid, w, -1.0)))] = True
        mean = unit((w[new_kept, None] * d[new_kept]).sum(axis=0))
        if np.array_equal(new_kept, kept):
            break
        kept = new_kept
    angles = np.degrees(np.arccos(np.clip(d[kept] @ mean, -1.0, 1.0)))
    dispersion = float(math.sqrt(np.average(angles**2, weights=w[kept])))
    return mean, dispersion, kept
