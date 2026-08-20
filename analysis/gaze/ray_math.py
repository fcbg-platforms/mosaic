"""
Pure numpy 3D gaze-ray math for the Multi-Camera Gaze Fusion plugin
(analysis/run_gaze_fusion.py). Zero cv2/mediapipe imports — this is the
"pure logic, highest-value to unit test" slice of the pipeline, the same
isolation discipline as expression/classifier.py's classify_expression()
and diarize/pipeline.py's assign_speakers(), kept importable and testable
without either heavy dependency installed. See analysis/tests/test_gaze_ray_math.py.

All positions/lengths are in millimetres; all "extrinsic_rt" arguments are
the same row-major 4x4 homogeneous-transform convention used on the C++
side (room_frame::Mat4 / CalibrationData::extrinsicRt): point_room = R *
point_local + t, camera 0 always identity.
"""

from __future__ import annotations

import numpy as np


def camera_ray_from_pose(
    rotation: np.ndarray,
    translation: np.ndarray,
    gaze_dx: float,
    gaze_dy: float,
    eye_origin_model_mm: np.ndarray,
    max_eye_yaw_deg: float = 30.0,
    max_eye_pitch_deg: float = 20.0,
) -> tuple[np.ndarray, np.ndarray]:
    """Turn a solved head pose plus a 2D iris-offset into a 3D camera-space gaze ray.

    Parameters
    ----------
    rotation : numpy.ndarray
        3x3 head-pose rotation, camera-local (from ``cv2.solvePnP``).
    translation : numpy.ndarray
        Length-3 head-pose translation, camera-local, mm.
    gaze_dx, gaze_dy : float
        2D iris-offset heuristic, each in ``[-1, 1]`` — same convention
        as ``python/pose/gaze_estimator.py``'s ``GazeResult``.
    eye_origin_model_mm : numpy.ndarray
        Length-3 eye-center point in the generic face model's own
        coordinates, mm (see ``estimator.py``'s ``EYE_ORIGIN_MODEL_MM``).
    max_eye_yaw_deg : float, default 30.0
        Eye-in-socket yaw bound, applied when ``gaze_dx = ±1``.
    max_eye_pitch_deg : float, default 20.0
        Eye-in-socket pitch bound, applied when ``gaze_dy = ±1``.

    Returns
    -------
    tuple of (numpy.ndarray, numpy.ndarray)
        ``(origin, direction)`` — the ray's origin (mm) and unit
        direction, both in the **camera's local** coordinate frame.
        ``gaze_dx = gaze_dy = 0`` returns the head's own forward
        direction, unperturbed.

    Notes
    -----
    The direction composes the head's own forward axis (+Z of the face
    model, metric — solved via ``cv2.solvePnP`` against the camera's real
    intrinsics) with a small eye-in-socket yaw/pitch perturbation derived
    from ``gaze_dx``/``gaze_dy``. This deliberately does not claim true
    stereo eye depth (unobservable from monocular iris landmarks) — it
    only separates "which way is the head pointing" (metric, solved) from
    "which way are the eyes rotated within it" (heuristic, bounded by
    ``max_eye_yaw_deg``/``max_eye_pitch_deg``), which is strictly more
    information than the live 2D-only ``gaze_dx``/``gaze_dy`` estimator
    this pipeline replaces. See :doc:`/math/gaze_fusion` for the full
    derivation.
    """
    rotation = np.asarray(rotation, dtype=np.float64)
    translation = np.asarray(translation, dtype=np.float64)
    eye_origin_model_mm = np.asarray(eye_origin_model_mm, dtype=np.float64)

    origin = rotation @ eye_origin_model_mm + translation

    yaw = np.radians(gaze_dx * max_eye_yaw_deg)
    pitch = np.radians(gaze_dy * max_eye_pitch_deg)

    cos_y, sin_y = np.cos(yaw), np.sin(yaw)
    cos_p, sin_p = np.cos(pitch), np.sin(pitch)
    rot_yaw = np.array([[cos_y, 0.0, sin_y], [0.0, 1.0, 0.0], [-sin_y, 0.0, cos_y]])
    rot_pitch = np.array([[1.0, 0.0, 0.0], [0.0, cos_p, -sin_p], [0.0, sin_p, cos_p]])

    forward_model = np.array([0.0, 0.0, 1.0])
    direction_model = rot_yaw @ (rot_pitch @ forward_model)
    direction = rotation @ direction_model
    direction = direction / np.linalg.norm(direction)

    return origin, direction


def transform_ray_to_room(
    origin_cam: np.ndarray, direction_cam: np.ndarray, extrinsic_rt
) -> tuple[np.ndarray, np.ndarray]:
    """Transform a camera-local ray into room coordinates.

    Parameters
    ----------
    origin_cam : numpy.ndarray
        Ray origin, camera-local, mm.
    direction_cam : numpy.ndarray
        Ray unit direction, camera-local.
    extrinsic_rt : array_like
        This camera's extrinsic pose: a flat length-16 sequence or a 4x4
        ``numpy.ndarray`` (row-major — see the module docstring's
        convention note).

    Returns
    -------
    tuple of (numpy.ndarray, numpy.ndarray)
        ``(origin_room, direction_room)`` — the same ray, in room
        coordinates.
    """
    m = np.asarray(extrinsic_rt, dtype=np.float64).reshape(4, 4)
    rot = m[:3, :3]
    t = m[:3, 3]

    origin_cam = np.asarray(origin_cam, dtype=np.float64)
    direction_cam = np.asarray(direction_cam, dtype=np.float64)

    origin_room = rot @ origin_cam + t
    direction_room = rot @ direction_cam
    direction_room = direction_room / np.linalg.norm(direction_room)
    return origin_room, direction_room


def closest_point_of_rays(origins, directions) -> tuple[np.ndarray, float]:
    """Triangulate the point closest to a set of 3D rays (least squares).

    Parameters
    ----------
    origins : sequence of array_like
        One ray origin per contributing camera, room coordinates, mm.
    directions : sequence of array_like
        One ray direction per contributing camera (need not be
        pre-normalized — renormalized internally), room coordinates.

    Returns
    -------
    tuple of (numpy.ndarray, float)
        ``(point, residual_rms)`` — the fused 3D point and the RMS
        perpendicular distance from it to each contributing ray (always
        ``0.0`` for a single ray, which trivially "fuses" to a point on
        itself).

    Notes
    -----
    Standard closest-point-of-multiple-rays least squares: minimizes
    :math:`\\sum_i \\lVert (I - d_i d_i^\\mathsf{T})(x - o_i) \\rVert^2`,
    i.e. the point whose summed squared perpendicular distance to every
    ray is smallest. Falls back to a pseudo-inverse (rather than raising)
    when the accumulated normal matrix is near-singular — e.g. all rays
    nearly parallel — so a genuinely degenerate configuration still
    returns a best-effort point; the resulting (large) ``residual_rms``
    is what should flag it as untrustworthy to a caller, not an
    exception. See :doc:`/math/gaze_fusion` for the full derivation.
    """
    origins = [np.asarray(o, dtype=np.float64) for o in origins]
    directions = [np.asarray(d, dtype=np.float64) / np.linalg.norm(d) for d in directions]

    if len(origins) == 1:
        return origins[0].copy(), 0.0

    a = np.zeros((3, 3))
    b = np.zeros(3)
    for o, d in zip(origins, directions, strict=False):
        proj = np.eye(3) - np.outer(d, d)
        a += proj
        b += proj @ o

    try:
        point = np.linalg.solve(a, b)
    except np.linalg.LinAlgError:
        point = np.linalg.pinv(a) @ b

    sq_errs = []
    for o, d in zip(origins, directions, strict=False):
        proj = np.eye(3) - np.outer(d, d)
        residual_vec = proj @ (point - o)
        sq_errs.append(float(np.dot(residual_vec, residual_vec)))
    residual_rms = float(np.sqrt(np.mean(sq_errs)))

    return point, residual_rms


def ray_plane_intersection(origin, direction, plane_point, plane_normal, eps: float = 1e-9):
    """Intersect a ray with a plane.

    Parameters
    ----------
    origin : array_like
        Ray origin, room coordinates, mm.
    direction : array_like
        Ray direction (need not be pre-normalized).
    plane_point : array_like
        Any point on the plane, room coordinates, mm.
    plane_normal : array_like
        Plane normal (need not be pre-normalized — renormalized
        internally).
    eps : float, default 1e-9
        Below this, ``direction`` is treated as parallel to the plane.

    Returns
    -------
    numpy.ndarray or None
        The 3D point where the ray (``origin + t*direction``, ``t >= 0``)
        intersects the plane, or ``None`` if the ray is (near-)parallel
        to the plane or the intersection lies behind the ray's origin
        (``t < 0`` — gaze pointing away from the surface). See
        :doc:`/math/gaze_fusion` for the derivation.
    """
    origin = np.asarray(origin, dtype=np.float64)
    direction = np.asarray(direction, dtype=np.float64)
    plane_point = np.asarray(plane_point, dtype=np.float64)
    plane_normal = np.asarray(plane_normal, dtype=np.float64)
    plane_normal = plane_normal / np.linalg.norm(plane_normal)

    denom = float(np.dot(direction, plane_normal))
    if abs(denom) < eps:
        return None

    t = float(np.dot(plane_point - origin, plane_normal) / denom)
    if t < 0:
        return None

    return origin + t * direction
