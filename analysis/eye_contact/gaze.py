"""
Gaze direction from one camera, per frame.

The 3D gaze plugin's geometric eyeball model (:mod:`gaze.eye_model`) on a
single view: head pose places each eyeball centre in the camera frame, the
iris pixel is back-projected onto the iris sphere, and centre-to-iris plus
the kappa angle is the eye's visual axis. The two eyes count equally; one eye
much less reliable than the other (closed, or behind the nose) is left out.
Both eyes closed (a blink) gives no gaze.

Directions are reported as angles in the **camera frame**: yaw positive
towards image right (the subject's left when they face the camera), pitch
positive up, ``(0, 0)`` straight along the camera's axis towards it. Where the
partner is in those angles is found from the data (:mod:`eye_contact.metrics`),
so constant errors of the eye model (kappa varies by person, an uncalibrated
lens) shift the partner's direction and the gaze together and cancel.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

#: One eye is used alone when the other's weight is below this share of it.
ONE_EYE_RATIO = 0.25


@dataclass
class FrameGaze:
    """Gaze of one frame (camera frame)."""

    origin: np.ndarray  # mm, between the eyes (or the eye used)
    direction: np.ndarray  # unit
    weight: float  # reliability, 0..1
    sigma_deg: float  # expected noise from iris resolution


def gaze_angles(direction) -> tuple[float, float]:
    """``(yaw, pitch)`` in degrees of a camera-frame direction (see the
    module docstring)."""
    d = np.asarray(direction, dtype=np.float64)
    yaw = math.degrees(math.atan2(d[0], -d[2]))
    pitch = math.degrees(math.atan2(-d[1], math.hypot(d[0], d[2])))
    return yaw, pitch


def direction_from_angles(yaw_deg: float, pitch_deg: float) -> np.ndarray:
    """Inverse of :func:`gaze_angles`."""
    y, p = math.radians(yaw_deg), math.radians(pitch_deg)
    return np.array([math.cos(p) * math.sin(y), -math.sin(p), -math.cos(p) * math.cos(y)])


class GazeModel:
    """Per-frame gaze from 478 landmarks and a camera-frame head pose."""

    def __init__(self, camera_matrix, dist_coeffs) -> None:
        from gaze.canonical import load_canonical_face
        from gaze.face_detect import landmarker_path

        self._canonical = load_canonical_face(landmarker_path())
        self._k = np.asarray(camera_matrix, dtype=np.float64).reshape(3, 3)
        self._d = np.asarray(dist_coeffs, dtype=np.float64).reshape(-1)

    def gaze(self, px: np.ndarray, pose) -> FrameGaze | None:
        import cv2
        from gaze.canonical import LEFT_IRIS, RIGHT_IRIS
        from gaze.eye_model import EYE_CENTRE_DEPTH_MM, EYES, estimate_eye, eye_openness

        if pose is None or px is None or len(px) < 478:
            return None
        found = {}
        for name, (outer, inner, top, bottom, nasal) in EYES.items():
            iris = RIGHT_IRIS if name == "right" else LEFT_IRIS
            mid = (self._canonical.vertices_mm[outer] + self._canonical.vertices_mm[inner]) / 2.0
            centre_cam = pose.apply(mid + np.array([0.0, 0.0, EYE_CENTRE_DEPTH_MM]))
            centre_px = px[iris[0]]
            iris_r = float(np.mean(np.linalg.norm(px[list(iris[1:])] - centre_px, axis=1)))
            openness = eye_openness(px[outer], px[inner], px[top], px[bottom])
            norm = cv2.undistortPoints(
                centre_px.reshape(1, 1, 2).astype(np.float64), self._k, self._d
            ).reshape(2)
            est = estimate_eye(
                pose.r, centre_cam, np.array([norm[0], norm[1], 1.0]), nasal, openness, iris_r
            )
            if est.weight > 0.0:
                found[name] = est
        if not found:
            return None
        best = max(found.values(), key=lambda e: e.weight)
        used = [e for e in found.values() if e.weight >= ONE_EYE_RATIO * best.weight]
        direction = sum(e.visual_axis_cam for e in used)
        direction = direction / np.linalg.norm(direction)
        origin = sum(e.centre_cam for e in used) / len(used)
        weight = float(np.mean([e.weight for e in used]))
        sigma = float(math.sqrt(sum(e.sigma_deg**2 for e in used)) / len(used))
        return FrameGaze(origin, direction, weight, sigma)
