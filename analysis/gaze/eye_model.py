"""
Geometric eyeball model: from an iris pixel to the eye's visual axis.

Each eye is a sphere rotating about a centre fixed in the head. Head pose
(:mod:`gaze.head_pose`) places that centre in the camera frame; the iris
centre seen in the image lies on the camera ray through its pixel, and on a
sphere of radius :data:`IRIS_SPHERE_RADIUS_MM` around the eyeball centre.
Intersecting the two gives the iris in 3D, and centre-to-iris is the eye's
optical axis. The visual axis (what the person actually fixates) differs
from it by the angle kappa, about 5 degrees towards the nose and 1.5 degrees
up in adults.

Head rotation and eye rotation are therefore separated properly: turning the
head moves the eyeball centre and the iris together and changes nothing in
the eye; only the iris moving relative to the centre does.

Numpy only. Pixel undistortion happens in the caller, which passes
normalised camera rays.

Anatomy behind the constants, relative to the MediaPipe canonical face (see
:mod:`gaze.canonical`): the eyelid surface sits about 3.6 mm in front of the
eye-corner midpoint; the eye's rotation centre is about 13 mm behind the
corneal apex and the pupil plane about 3.5 mm behind it. Hence a centre
:data:`EYE_CENTRE_DEPTH_MM` behind the corner midpoint and an iris
:data:`IRIS_SPHERE_RADIUS_MM` in front of the centre.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from .canonical import (
    LEFT_EYE_BOTTOM,
    LEFT_EYE_INNER,
    LEFT_EYE_OUTER,
    LEFT_EYE_TOP,
    RIGHT_EYE_BOTTOM,
    RIGHT_EYE_INNER,
    RIGHT_EYE_OUTER,
    RIGHT_EYE_TOP,
)
from .ray_math import head_direction_from_yaw_pitch, unit, yaw_pitch_from_head_direction

#: Eyeball rotation centre, behind (head +z) the midpoint of the two eye corners.
EYE_CENTRE_DEPTH_MM = 8.0
#: Distance from the rotation centre to the iris (pupil) centre.
IRIS_SPHERE_RADIUS_MM = 9.5
#: Visual axis relative to the optical axis, average adult values.
KAPPA_NASAL_DEG = 5.0
KAPPA_UP_DEG = 1.5
#: Lid gap / eye width below which the eye counts as closed (a blink).
EYE_CLOSED_RATIO = 0.12
#: Iris rim-to-rim landmark noise assumed when converting pixels to an
#: angular uncertainty (MediaPipe iris landmarks, 1 px is optimistic).
IRIS_PIXEL_NOISE_PX = 1.0
#: Largest eye-in-head rotation accepted, degrees. The eyes can turn about
#: 45 degrees but rarely go past 30: people turn the head instead. An iris
#: estimate further out is almost always pixel noise on a small iris (the ray
#: just missing the eyeball reads as looking 90 degrees sideways), so it is
#: pulled back to this limit and trusted much less.
MAX_EYE_ROTATION_DEG = 35.0
#: How far each eye's own forward direction turns outward from the head's.
EYE_OUTWARD_DEG = 30.0
#: Average human iris diameter (horizontal visible iris), mm.
IRIS_DIAMETER_MM = 11.7

EYES = {
    # name: (outer, inner, top, bottom, nasal direction along head +x)
    "right": (RIGHT_EYE_OUTER, RIGHT_EYE_INNER, RIGHT_EYE_TOP, RIGHT_EYE_BOTTOM, +1.0),
    "left": (LEFT_EYE_OUTER, LEFT_EYE_INNER, LEFT_EYE_TOP, LEFT_EYE_BOTTOM, -1.0),
}


def eyeball_centre_head(corner_outer, corner_inner) -> np.ndarray:
    """Eyeball rotation centre in the head frame.

    Parameters
    ----------
    corner_outer, corner_inner : array_like
        The eye's two corners in the head frame (mm), usually canonical
        model points scaled by the subject's face scale.

    Returns
    -------
    numpy.ndarray
        ``(3,)``, :data:`EYE_CENTRE_DEPTH_MM` behind the corners' midpoint.
    """
    mid = (np.asarray(corner_outer, dtype=np.float64) + np.asarray(corner_inner)) / 2.0
    return mid + np.array([0.0, 0.0, EYE_CENTRE_DEPTH_MM])


def iris_on_sphere(ray_dir, centre, radius: float = IRIS_SPHERE_RADIUS_MM):
    """Where the camera ray through the iris pixel meets the iris sphere.

    Parameters
    ----------
    ray_dir : array_like
        Direction of the camera ray (from the camera centre, camera frame).
    centre : array_like
        Eyeball centre in the camera frame (mm).
    radius : float
        Iris sphere radius (mm).

    Returns
    -------
    tuple
        ``(point, hit)``: the iris centre in the camera frame, and whether
        the ray actually met the sphere. On a miss (pixel noise or a model
        error pushed the ray past the eye's silhouette) the point is the
        sphere's tangent point nearest the ray, so the result degrades to
        "looking sideways" instead of failing.
    """
    r = unit(ray_dir)
    c = np.asarray(centre, dtype=np.float64)
    rc = float(np.dot(r, c))
    disc = rc * rc - float(np.dot(c, c)) + radius * radius
    if disc >= 0.0:
        t = rc - math.sqrt(disc)  # the near (front) intersection
        return t * r, True
    closest = rc * r
    return c + radius * unit(closest - c), False


def visual_axis(optical_axis_head, nasal_sign: float) -> np.ndarray:
    """Turn an optical axis into the visual axis, both in the head frame.

    Parameters
    ----------
    optical_axis_head : array_like
        Centre-to-iris direction in the head frame.
    nasal_sign : float
        +1 when the nose lies towards head +x from this eye (the subject's
        right eye), -1 for the left eye.

    Returns
    -------
    numpy.ndarray
        Unit visual axis, :data:`KAPPA_NASAL_DEG` towards the nose and
        :data:`KAPPA_UP_DEG` up from the optical axis.
    """
    yaw, pitch = yaw_pitch_from_head_direction(optical_axis_head)
    yaw += nasal_sign * math.radians(KAPPA_NASAL_DEG)
    pitch += math.radians(KAPPA_UP_DEG)
    return head_direction_from_yaw_pitch(yaw, pitch)


def limit_eye_rotation(optical_head, max_deg: float = MAX_EYE_ROTATION_DEG):
    """Pull an optical axis (head frame) back inside the eye's rotation range.

    Returns
    -------
    tuple
        ``(axis, clamped)``: the axis, at most ``max_deg`` from straight
        ahead (head -z), and whether it had to be moved.
    """
    d = unit(optical_head)
    forward = np.array([0.0, 0.0, -1.0])
    angle = math.degrees(math.acos(max(-1.0, min(1.0, float(np.dot(d, forward))))))
    if angle <= max_deg:
        return d, False
    side = d - np.dot(d, forward) * forward
    if np.linalg.norm(side) < 1e-9:
        return forward, True
    m = math.radians(max_deg)
    return unit(math.cos(m) * forward + math.sin(m) * unit(side)), True


def eye_openness(px_outer, px_inner, px_top, px_bottom) -> float:
    """Lid gap divided by eye width, from image points (any consistent units)."""
    width = float(np.linalg.norm(np.asarray(px_outer) - np.asarray(px_inner)))
    if width < 1e-6:
        return 0.0
    return float(np.linalg.norm(np.asarray(px_top) - np.asarray(px_bottom))) / width


@dataclass
class EyeEstimate:
    """One eye seen by one camera.

    Attributes
    ----------
    centre_cam : numpy.ndarray
        Eyeball centre, camera frame (mm).
    visual_axis_cam : numpy.ndarray
        Unit visual axis, camera frame.
    weight : float
        Reliability in ``[0, 1]`` (0 for a closed or unusable eye).
    sigma_deg : float
        Expected angular noise of this estimate from iris pixel resolution.
    """

    centre_cam: np.ndarray
    visual_axis_cam: np.ndarray
    weight: float
    sigma_deg: float


def estimate_eye(
    r_cam_head,
    centre_cam,
    iris_ray_cam,
    nasal_sign: float,
    openness: float,
    iris_radius_px: float,
) -> EyeEstimate:
    """Visual axis of one eye from one camera.

    Parameters
    ----------
    r_cam_head : array_like
        ``(3, 3)`` rotation, head frame to camera frame.
    centre_cam : array_like
        Eyeball centre in the camera frame (mm).
    iris_ray_cam : array_like
        Camera ray through the undistorted iris-centre pixel,
        ``(x_n, y_n, 1)``.
    nasal_sign : float
        See :func:`visual_axis`.
    openness : float
        :func:`eye_openness` of this eye in this camera.
    iris_radius_px : float
        Iris radius in pixels (from the iris rim landmarks), the image
        resolution available for this eye.

    Returns
    -------
    EyeEstimate
    """
    r = np.asarray(r_cam_head, dtype=np.float64).reshape(3, 3)
    c = np.asarray(centre_cam, dtype=np.float64)
    iris, hit = iris_on_sphere(iris_ray_cam, c)
    optical_head, clamped = limit_eye_rotation(r.T @ unit(iris - c))
    axis_cam = r @ visual_axis(optical_head, nasal_sign)

    # How far the iris can move in pixels for the eye's full rotation range:
    # the iris sphere radius seen at this distance. One pixel of landmark
    # noise is then about atan(noise / that radius) of eye rotation.
    sphere_px = max(iris_radius_px * IRIS_SPHERE_RADIUS_MM / (IRIS_DIAMETER_MM / 2.0), 1e-3)
    sigma = math.degrees(math.atan(IRIS_PIXEL_NOISE_PX / sphere_px))

    # Reliability: resolution, whether this eye faces the camera, lid state.
    # Eyes face about 30 degrees outward from the head's forward direction,
    # so a head turned away puts the far eye behind the nose first.
    eye_normal_head = head_direction_from_yaw_pitch(
        -nasal_sign * math.radians(EYE_OUTWARD_DEG), 0.0
    )
    facing = float(np.dot(r @ eye_normal_head, unit(-c)))
    w_facing = max(0.0, (facing - 0.17) / 0.83) ** 2  # zero beyond ~80 degrees
    w_res = min(1.0, max(0.0, (iris_radius_px - 1.0) / 4.0))
    w_open = 0.0 if openness < EYE_CLOSED_RATIO else min(1.0, openness / 0.25)
    weight = w_facing * w_res * w_open * (1.0 if hit else 0.3) * (0.3 if clamped else 1.0)
    return EyeEstimate(centre_cam=c, visual_axis_cam=axis_cam, weight=weight, sigma_deg=sigma)
