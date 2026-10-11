"""
2D gaze from MediaPipe face landmarks: how far each iris sits from the middle
of its eye, and whether that counts as looking at the camera.

This file exists twice, byte for byte: python/pose/iris_offset.py (the live
gaze arrow and on-target figure) and analysis/gaze2d/iris_offset.py (the
Gaze 2D plugin). The two are separate uv projects, so neither can import the
other; analysis/tests/test_iris_offset_shared.py fails if the copies differ,
or if the on-target rule stops matching the C++ one in
src/analysis/realtime_metrics.hpp (gaze_on_target_for). Edit both copies
together. Pure Python: no mediapipe, numpy or OpenCV import.
"""

from __future__ import annotations

import math

# MediaPipe FaceLandmarker's 478-point mesh (468 face points plus 5 per iris).
LEFT_IRIS_CENTER, RIGHT_IRIS_CENTER = 468, 473
LEFT_EYE_OUTER, LEFT_EYE_INNER = 33, 133
LEFT_EYE_TOP, LEFT_EYE_BOTTOM = 159, 145
RIGHT_EYE_INNER, RIGHT_EYE_OUTER = 362, 263
RIGHT_EYE_TOP, RIGHT_EYE_BOTTOM = 386, 374

#: A gaze offset within this distance of (0, 0) counts as looking at the
#: camera. Same value and rule (a radius, not a box) as
#: kGazeOnTargetThreshold / gaze_on_target_for() in realtime_metrics.hpp.
GAZE_ON_TARGET_THRESHOLD = 0.35


def compute_iris_offset(landmarks) -> tuple[float, float] | tuple[None, None]:
    """The iris offset of one face, averaged over both eyes.

    Parameters
    ----------
    landmarks : sequence
        The 478 face landmarks of one face, each with ``.x`` and ``.y`` in
        normalised image coordinates.

    Returns
    -------
    tuple of (float, float) or tuple of (None, None)
        ``(gaze_dx, gaze_dy)``: each iris centre's offset from the middle of
        its eye (corner to corner, lid to lid), divided by the mean eye width,
        averaged over both eyes and clamped to ``[-1, 1]``. Positive is right
        and down in the image. ``(None, None)`` when the eyes are too narrow
        to divide by (a degenerate detection).
    """
    li_x, li_y = landmarks[LEFT_IRIS_CENTER].x, landmarks[LEFT_IRIS_CENTER].y
    ri_x, ri_y = landmarks[RIGHT_IRIS_CENTER].x, landmarks[RIGHT_IRIS_CENTER].y

    left_cx = (landmarks[LEFT_EYE_OUTER].x + landmarks[LEFT_EYE_INNER].x) / 2
    left_cy = (landmarks[LEFT_EYE_TOP].y + landmarks[LEFT_EYE_BOTTOM].y) / 2
    left_w = abs(landmarks[LEFT_EYE_OUTER].x - landmarks[LEFT_EYE_INNER].x)

    right_cx = (landmarks[RIGHT_EYE_INNER].x + landmarks[RIGHT_EYE_OUTER].x) / 2
    right_cy = (landmarks[RIGHT_EYE_TOP].y + landmarks[RIGHT_EYE_BOTTOM].y) / 2
    right_w = abs(landmarks[RIGHT_EYE_INNER].x - landmarks[RIGHT_EYE_OUTER].x)

    eye_w = (left_w + right_w) / 2
    if eye_w < 1e-6:
        return None, None

    dx = ((li_x - left_cx) / eye_w + (ri_x - right_cx) / eye_w) / 2
    dy = ((li_y - left_cy) / eye_w + (ri_y - right_cy) / eye_w) / 2
    return float(max(-1.0, min(1.0, dx))), float(max(-1.0, min(1.0, dy)))


def gaze_on_target(
    gaze_dx: float, gaze_dy: float, threshold: float = GAZE_ON_TARGET_THRESHOLD
) -> bool:
    """Whether an iris offset counts as looking at the camera: within
    ``threshold`` of ``(0, 0)``."""
    return math.hypot(gaze_dx, gaze_dy) <= threshold
