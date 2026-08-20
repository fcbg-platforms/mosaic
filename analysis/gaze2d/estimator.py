"""
Calibration-free, single-camera 2D gaze estimation for the post-hoc
Analysis-tab plugin (analysis/run_gaze2d.py).

Reuses the exact iris-centre-offset-from-eye-socket-centre heuristic
already running live in the Real-time tab (python/pose/gaze_estimator.py's
GazeEstimator.estimate()) and in the Multi-Camera Gaze Fusion plugin
(analysis/gaze/estimator.py's _iris_offset()) — this is deliberately a
THIRD copy of that same heuristic, not a shared import, matching this
project's own already-documented cross-project/cross-plugin boundary rule
(see analysis/gaze/estimator.py's own module docstring for the full
rationale: python/ and analysis/ are separate uv projects with
independent dependency trees, and reaching across plugin folders within
analysis/ itself is avoided the same way for consistency).

Unlike analysis/gaze/estimator.py (needs a camera's real intrinsics for
cv2.solvePnP, to produce a metric 3D gaze RAY) or analysis/run_gaze_fusion.py
(needs room extrinsics too, to fuse rays across cameras), this module needs
NO calibration data at all — it produces the same normalized [-1, 1]
gaze_dx/gaze_dy heuristic the live path already does, per camera, per
frame. This is what makes the plugin built on top of it "calibration-free":
no intrinsic or room/extrinsic calibration step is a prerequisite.
"""

from __future__ import annotations

import urllib.request
from dataclasses import dataclass
from pathlib import Path

import numpy as np

_MODELS_DIR = Path(__file__).parent / "models"

_MEDIAPIPE_MODEL_URL = (
    "https://storage.googleapis.com/mediapipe-models/"
    "face_landmarker/face_landmarker/float16/1/face_landmarker.task"
)

# ── Landmark indices ─────────────────────────────────────────────────────
# Same MediaPipe FaceLandmarker 478-point mesh python/pose/gaze_estimator.py
# and analysis/gaze/estimator.py both use for this identical heuristic;
# duplicated rather than imported (see module docstring).
_LEFT_IRIS_CENTER, _RIGHT_IRIS_CENTER = 468, 473
_LEFT_EYE_OUTER, _LEFT_EYE_INNER = 33, 133
_LEFT_EYE_TOP, _LEFT_EYE_BOTTOM = 159, 145
_RIGHT_EYE_INNER, _RIGHT_EYE_OUTER = 362, 263
_RIGHT_EYE_TOP, _RIGHT_EYE_BOTTOM = 386, 374


@dataclass
class Gaze2dSample:
    """One detected face's pixel-space bounding box and 2D gaze heuristic.

    Attributes
    ----------
    bbox_xyxy : tuple of float
        Detection bounding box, ``(x1, y1, x2, y2)`` PIXELS — not the
        normalized [0,1] convention the live real-time path uses, matching
        the pixel-space convention already established by RppgFrame's/
        ExpressionSubject's own bbox fields in this codebase.
    gaze_dx, gaze_dy : float
        Iris-centre-offset-from-eye-socket-centre heuristic, each in
        ``[-1, 1]``. See ``compute_iris_offset()`` for the exact formula.
    """

    bbox_xyxy: tuple[float, float, float, float]
    gaze_dx: float
    gaze_dy: float


class Gaze2dEstimator:
    """MediaPipe Tasks ``FaceLandmarker``-based single-face 2D gaze estimator.

    Parameters
    ----------
    min_confidence : float, default 0.5
        Minimum face-detection/-presence/-tracking confidence to keep a
        detection.
    """

    def __init__(self, min_confidence: float = 0.5) -> None:
        import mediapipe as mp
        from mediapipe.tasks import python as mp_python
        from mediapipe.tasks.python import vision as mp_vision

        model_path = _ensure_download(_MODELS_DIR / "face_landmarker.task", _MEDIAPIPE_MODEL_URL)

        options = mp_vision.FaceLandmarkerOptions(
            base_options=mp_python.BaseOptions(model_asset_path=str(model_path)),
            running_mode=mp_vision.RunningMode.IMAGE,
            num_faces=1,  # calibration-free 2D gaze only ever tracks one subject per camera
            min_face_detection_confidence=min_confidence,
            min_face_presence_confidence=min_confidence,
            min_tracking_confidence=min_confidence,
            output_face_blendshapes=False,
            output_facial_transformation_matrixes=False,
        )
        self._mp = mp
        self._landmarker = mp_vision.FaceLandmarker.create_from_options(options)

    def estimate(self, frame_bgr: np.ndarray) -> Gaze2dSample | None:
        """Detect a face and compute its 2D gaze heuristic in one BGR frame.

        Parameters
        ----------
        frame_bgr : numpy.ndarray
            BGR frame, as returned by ``cv2.VideoCapture``.

        Returns
        -------
        Gaze2dSample or None
            ``None`` if no face was detected, the model output lacks iris
            landmarks, or the eye width is too small to normalise by — a
            real gap, never fabricated (matches this codebase's
            established "skip missing samples" discipline).
        """
        import cv2

        h, w = frame_bgr.shape[:2]
        rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        mp_image = self._mp.Image(image_format=self._mp.ImageFormat.SRGB, data=rgb)
        result = self._landmarker.detect(mp_image)

        if not result.face_landmarks:
            return None

        lm = result.face_landmarks[0]
        if len(lm) < 478:
            return None  # iris landmarks not present in this model output

        gaze_dx, gaze_dy = compute_iris_offset(lm)
        if gaze_dx is None:
            return None

        xs = [p.x for p in lm[:468]]
        ys = [p.y for p in lm[:468]]
        bbox = (min(xs) * w, min(ys) * h, max(xs) * w, max(ys) * h)

        return Gaze2dSample(bbox_xyxy=bbox, gaze_dx=gaze_dx, gaze_dy=gaze_dy)


def compute_iris_offset(landmarks) -> tuple[float, float] | tuple[None, None]:
    """Compute the 2D iris-centre-offset-from-eye-socket-centre heuristic.

    Parameters
    ----------
    landmarks : sequence
        MediaPipe FaceLandmarker's 478-point face-mesh landmark list for one
        detected face (each entry exposing ``.x``/``.y`` in [0,1] normalized
        image coordinates).

    Returns
    -------
    tuple of (float, float) or tuple of (None, None)
        ``(gaze_dx, gaze_dy)``, each clamped to ``[-1, 1]``, or
        ``(None, None)`` if the eye width is too small to normalise by
        (a degenerate detection).

    Notes
    -----
    Identical heuristic to ``python/pose/gaze_estimator.py``'s
    ``GazeEstimator.estimate()`` and ``analysis/gaze/estimator.py``'s
    ``_iris_offset()`` — iris-centre offset from eye-socket centre,
    normalised by eye width, averaged across both eyes. Pulled out as a
    real, non-underscore-prefixed, directly testable function (unlike its
    two siblings, which keep this private) specifically because neither of
    those two existing copies has ever had a dedicated unit test despite
    being duplicated twice already — see
    analysis/tests/test_gaze2d_estimator.py.
    """
    li_x, li_y = landmarks[_LEFT_IRIS_CENTER].x, landmarks[_LEFT_IRIS_CENTER].y
    ri_x, ri_y = landmarks[_RIGHT_IRIS_CENTER].x, landmarks[_RIGHT_IRIS_CENTER].y

    left_cx = (landmarks[_LEFT_EYE_OUTER].x + landmarks[_LEFT_EYE_INNER].x) / 2
    left_cy = (landmarks[_LEFT_EYE_TOP].y + landmarks[_LEFT_EYE_BOTTOM].y) / 2
    left_w = abs(landmarks[_LEFT_EYE_OUTER].x - landmarks[_LEFT_EYE_INNER].x)

    right_cx = (landmarks[_RIGHT_EYE_INNER].x + landmarks[_RIGHT_EYE_OUTER].x) / 2
    right_cy = (landmarks[_RIGHT_EYE_TOP].y + landmarks[_RIGHT_EYE_BOTTOM].y) / 2
    right_w = abs(landmarks[_RIGHT_EYE_INNER].x - landmarks[_RIGHT_EYE_OUTER].x)

    eye_w = (left_w + right_w) / 2
    if eye_w < 1e-6:
        return None, None

    dx = ((li_x - left_cx) / eye_w + (ri_x - right_cx) / eye_w) / 2
    dy = ((li_y - left_cy) / eye_w + (ri_y - right_cy) / eye_w) / 2
    return float(max(-1.0, min(1.0, dx))), float(max(-1.0, min(1.0, dy)))


def gaze_on_target(gaze_dx: float, gaze_dy: float, threshold: float = 0.35) -> bool:
    """Whether a (gaze_dx, gaze_dy) reading counts as "looking at target".

    Parameters
    ----------
    gaze_dx, gaze_dy : float
        Normalized gaze offset, each in ``[-1, 1]`` (see
        ``compute_iris_offset()``).
    threshold : float, default 0.35
        Maximum per-axis offset magnitude still considered "on target".

    Returns
    -------
    bool

    Notes
    -----
    Intentionally mirrors ``src/analysis/realtime_metrics.hpp``'s
    ``kGazeOnTargetThreshold = 0.35`` / ``gaze_on_target_for()`` — the two
    can't literally share code across the C++/Python boundary, so the
    threshold is duplicated here with this explicit cross-reference rather
    than silently risking the two drifting apart.
    """
    return abs(gaze_dx) <= threshold and abs(gaze_dy) <= threshold


def _ensure_download(dest: Path, url: str) -> Path:
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        print(f"[gaze2d] Downloading {dest.name} …", flush=True)
        urllib.request.urlretrieve(url, dest)
        print(f"[gaze2d] Downloaded to {dest}", flush=True)
    return dest
