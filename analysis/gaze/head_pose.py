"""
Metric head pose from MediaPipe landmarks and calibrated cameras.

Two steps:

1. **Per camera** (:func:`solve_pnp`): the canonical face's rigid landmarks
   (:mod:`gaze.canonical`) against their pixels, with the camera's real
   intrinsics and distortion. ``SOLVEPNP_SQPNP`` gives a global solution,
   refined by Levenberg-Marquardt.
2. **Across cameras** (:func:`refine_joint`): one rigid head in room
   coordinates that best explains the landmarks in *every* camera seeing
   the face, plus a face-scale factor. Real faces are up to about 8% larger
   or smaller than the canonical one, which a single camera cannot tell
   apart from distance; two or more cameras can. The scale learned for a
   subject is reused when only one camera sees them.

The robust loss keeps a single badly placed landmark (or a camera that
mislocated the face) from pulling the head off.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .ray_math import rotation_matrix_from_rotvec, rotvec_from_rotation_matrix


@dataclass
class HeadPose:
    """A head pose: ``p = r @ (scale * v_head) + t`` maps head-frame model
    points into the target frame (camera or room).

    Attributes
    ----------
    r : numpy.ndarray
        ``(3, 3)`` rotation.
    t : numpy.ndarray
        ``(3,)`` translation, mm.
    scale : float
        Face scale relative to the canonical face.
    rms_px : float
        Reprojection RMS over the landmarks used, pixels.
    """

    r: np.ndarray
    t: np.ndarray
    scale: float
    rms_px: float

    def apply(self, v_head) -> np.ndarray:
        """Map head-frame points ``(..., 3)`` into the pose's target frame."""
        v = np.asarray(v_head, dtype=np.float64)
        return (self.scale * v) @ self.r.T + self.t


def project_points(points_cam, camera_matrix, dist_coeffs) -> np.ndarray:
    """Project camera-frame points to distorted pixels (OpenCV's model).

    Parameters
    ----------
    points_cam : array_like
        ``(N, 3)`` points in the camera frame.
    camera_matrix : array_like
        ``(3, 3)`` intrinsics.
    dist_coeffs : array_like
        ``k1, k2, p1, p2[, k3]``.

    Returns
    -------
    numpy.ndarray
        ``(N, 2)`` pixel coordinates. Points at or behind the camera get
        ``nan``.

    Notes
    -----
    Pure numpy and identical to ``cv2.projectPoints`` for the 5-coefficient
    model (verified in the tests), so the joint fit below can evaluate it
    hundreds of times per frame cheaply.
    """
    p = np.asarray(points_cam, dtype=np.float64).reshape(-1, 3)
    k = np.asarray(camera_matrix, dtype=np.float64).reshape(3, 3)
    d = np.zeros(5)
    dc = np.asarray(dist_coeffs, dtype=np.float64).reshape(-1)
    d[: min(5, dc.size)] = dc[:5]
    z = p[:, 2]
    with np.errstate(divide="ignore", invalid="ignore"):
        x = np.where(z > 1e-9, p[:, 0] / z, np.nan)
        y = np.where(z > 1e-9, p[:, 1] / z, np.nan)
    r2 = x * x + y * y
    radial = 1.0 + d[0] * r2 + d[1] * r2 * r2 + d[4] * r2 * r2 * r2
    xd = x * radial + 2.0 * d[2] * x * y + d[3] * (r2 + 2.0 * x * x)
    yd = y * radial + d[2] * (r2 + 2.0 * y * y) + 2.0 * d[3] * x * y
    u = k[0, 0] * xd + k[0, 1] * yd + k[0, 2]
    v = k[1, 1] * yd + k[1, 2]
    return np.stack([u, v], axis=1)


def solve_pnp(model_pts, image_pts, camera_matrix, dist_coeffs) -> HeadPose | None:
    """Head pose in one camera's frame.

    Parameters
    ----------
    model_pts : array_like
        ``(N, 3)`` head-frame model points (already scaled), N >= 6.
    image_pts : array_like
        ``(N, 2)`` their pixels.
    camera_matrix, dist_coeffs : array_like
        The camera's real calibration.

    Returns
    -------
    HeadPose or None
        Camera-from-head pose with ``scale = 1`` (the scale is already in
        ``model_pts``), or ``None`` when the solve fails or puts the face
        behind the camera.
    """
    import cv2

    obj = np.asarray(model_pts, dtype=np.float64).reshape(-1, 3)
    img = np.asarray(image_pts, dtype=np.float64).reshape(-1, 2)
    if len(obj) < 6 or not np.isfinite(img).all():
        return None
    k = np.asarray(camera_matrix, dtype=np.float64).reshape(3, 3)
    dist = np.asarray(dist_coeffs, dtype=np.float64).reshape(-1)
    try:
        ok, rvec, tvec = cv2.solvePnP(obj, img, k, dist, flags=cv2.SOLVEPNP_SQPNP)
        if not ok:
            return None
        rvec, tvec = cv2.solvePnPRefineLM(obj, img, k, dist, rvec, tvec)
    except cv2.error:
        return None
    r = rotation_matrix_from_rotvec(rvec.reshape(3))
    t = tvec.reshape(3)
    pose = HeadPose(r=r, t=t, scale=1.0, rms_px=0.0)
    cam_pts = pose.apply(obj)
    if (cam_pts[:, 2] <= 0).any():
        return None
    proj = project_points(cam_pts, k, dist)
    pose.rms_px = float(np.sqrt(np.nanmean(np.sum((proj - img) ** 2, axis=1))))
    return pose


@dataclass
class CameraView:
    """One camera's view of one face, for :func:`refine_joint`.

    Attributes
    ----------
    camera_from_room : numpy.ndarray
        ``(4, 4)`` transform, room to camera.
    camera_matrix, dist_coeffs : numpy.ndarray
        The camera's calibration.
    model_pts : numpy.ndarray
        ``(N, 3)`` canonical head-frame points (unscaled).
    image_pts : numpy.ndarray
        ``(N, 2)`` their pixels in this camera.
    weights : numpy.ndarray
        ``(N,)`` per-landmark weights.
    """

    camera_from_room: np.ndarray
    camera_matrix: np.ndarray
    dist_coeffs: np.ndarray
    model_pts: np.ndarray
    image_pts: np.ndarray
    weights: np.ndarray


def refine_joint(
    views: list[CameraView],
    initial: HeadPose,
    fit_scale: bool = True,
    scale_bounds: tuple[float, float] = (0.8, 1.25),
    huber_px: float = 3.0,
) -> HeadPose:
    """One room-frame head pose explaining every camera's landmarks.

    Parameters
    ----------
    views : list of CameraView
        Every camera that sees this face (one is enough to refine; two or
        more are needed to learn the scale).
    initial : HeadPose
        Room-from-head starting point (e.g. one camera's PnP mapped to the
        room) and starting scale.
    fit_scale : bool, default True
        Also solve the face scale. Ignored with a single view, where scale
        and distance cannot be separated.
    scale_bounds : tuple of float
        Allowed scale range.
    huber_px : float
        Residual size (px) where the loss turns from quadratic to linear.

    Returns
    -------
    HeadPose
        Room-from-head pose, with ``rms_px`` over all views.
    """
    from scipy.optimize import least_squares

    fit_scale = fit_scale and len(views) >= 2
    x0 = np.concatenate(
        [rotvec_from_rotation_matrix(initial.r), initial.t, [np.log(initial.scale)]]
        if fit_scale
        else [rotvec_from_rotation_matrix(initial.r), initial.t]
    )

    def unpack(x):
        r = rotation_matrix_from_rotvec(x[:3])
        s = float(np.exp(x[6])) if fit_scale else initial.scale
        return r, x[3:6], s

    sqrt_w = [np.sqrt(np.asarray(v.weights, dtype=np.float64)) for v in views]

    def residuals(x):
        r, t, s = unpack(x)
        out = []
        for v, sw in zip(views, sqrt_w, strict=True):
            pts_room = (s * v.model_pts) @ r.T + t
            pts_cam = pts_room @ v.camera_from_room[:3, :3].T + v.camera_from_room[:3, 3]
            proj = project_points(pts_cam, v.camera_matrix, v.dist_coeffs)
            # A point pushed behind a camera mid-solve gets a large finite
            # residual instead of nan, steering the solver back.
            err = np.where(np.isfinite(proj), proj - v.image_pts, 1e3)
            out.append((err * sw[:, None]).ravel())
        return np.concatenate(out)

    lower = np.full(x0.shape, -np.inf)
    upper = np.full(x0.shape, np.inf)
    if fit_scale:
        lower[6], upper[6] = np.log(scale_bounds[0]), np.log(scale_bounds[1])
        x0[6] = np.clip(x0[6], lower[6] + 1e-9, upper[6] - 1e-9)
    sol = least_squares(
        residuals,
        x0,
        loss="huber",
        f_scale=huber_px,
        bounds=(lower, upper),
        x_scale="jac",
        max_nfev=400,
    )
    r, t, s = unpack(sol.x)
    pose = HeadPose(r=r, t=np.asarray(t, dtype=np.float64), scale=s, rms_px=0.0)
    res = residuals(sol.x).reshape(-1, 2)
    weights = np.concatenate([np.asarray(v.weights, dtype=np.float64) for v in views])
    pose.rms_px = float(np.sqrt(np.sum(res**2) / max(np.sum(weights), 1e-9)))
    return pose


def camera_pose_to_room(pose_cam: HeadPose, room_from_camera) -> HeadPose:
    """Re-express a camera-from-head pose as room-from-head."""
    m = np.asarray(room_from_camera, dtype=np.float64).reshape(4, 4)
    return HeadPose(
        r=m[:3, :3] @ pose_cam.r,
        t=m[:3, :3] @ pose_cam.t + m[:3, 3],
        scale=pose_cam.scale,
        rms_px=pose_cam.rms_px,
    )
