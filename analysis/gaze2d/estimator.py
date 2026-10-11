"""
Calibration-free, single-camera 2D gaze estimation for the post-hoc
Analysis-tab plugin (analysis/run_gaze2d.py).

Uses the same iris-centre-offset-from-eye-socket-centre heuristic as the
live gaze arrow in the Real-time tab: both come from iris_offset.py, a file
kept byte for byte identical in python/pose/ and here (python/ and analysis/
are separate uv projects, so neither can import the other; a test checks the
copies match).

Unlike the Multi-Camera Gaze Fusion plugin (analysis/run_gaze_fusion.py,
the analysis/gaze package), which needs each camera's intrinsics for a
metric head pose and an eyeball model, and room calibration to combine
cameras, this module needs NO calibration data at all — it produces the same normalized [-1, 1]
gaze_dx/gaze_dy heuristic the live path already does, per camera, per
frame. This is what makes the plugin built on top of it "calibration-free":
no intrinsic or room/extrinsic calibration step is a prerequisite.
"""

from __future__ import annotations

import urllib.request
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .iris_offset import compute_iris_offset, gaze_on_target

__all__ = ["Gaze2dEstimator", "Gaze2dSample", "compute_iris_offset", "gaze_on_target"]

_MODELS_DIR = Path(__file__).parent / "models"

_MEDIAPIPE_MODEL_URL = (
    "https://storage.googleapis.com/mediapipe-models/"
    "face_landmarker/face_landmarker/float16/1/face_landmarker.task"
)


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


def _ensure_download(dest: Path, url: str) -> Path:
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        print(f"[gaze2d] Downloading {dest.name} …", flush=True)
        urllib.request.urlretrieve(url, dest)
        print(f"[gaze2d] Downloaded to {dest}", flush=True)
    return dest
