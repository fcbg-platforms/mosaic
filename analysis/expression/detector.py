"""
Facial-expression detection backend — MediaPipe Tasks FaceLandmarker with
output_face_blendshapes=True.

Same model family/file as facemask/detectors.py's MediaPipeFaceDetector and
(in the separate python/ project) python/pose/gaze_estimator.py — but both
of those explicitly set output_face_blendshapes=False and this module
constructs its OWN FaceLandmarkerOptions instance rather than reusing
either, for the same reason facemask/detectors.py's docstring already gives
for not importing across the analysis/ vs python/ project boundary, applied
here to not reaching into a sibling plugin folder either. Owns its own model
cache under analysis/expression/models/ (gitignored), re-downloading the
same face_landmarker.task file facemask already has as an accepted
tradeoff.
"""

from __future__ import annotations

import urllib.request
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

_MODELS_DIR = Path(__file__).parent / "models"

_MEDIAPIPE_MODEL_URL = (
    "https://storage.googleapis.com/mediapipe-models/"
    "face_landmarker/face_landmarker/float16/1/face_landmarker.task"
)

#: The standard ARKit-style blendshape category names MediaPipe's
#: ``FaceLandmarker`` outputs when ``output_face_blendshapes=True`` (the
#: same category set Apple ARKit/most game engines use, plus MediaPipe's
#: own ``"_neutral"``). This module never trusts MediaPipe's positional
#: output order or exact count — score lookup is always by category
#: **name** against this list, so a future mediapipe version reordering
#: (or adding/removing) its output categories can't silently misalign
#: names/scores. Any category MediaPipe returns that isn't in this list is
#: silently omitted; any name listed here that MediaPipe doesn't return
#: defaults to ``0.0`` — both are documented, low-risk degrades, not
#: crashes.
BLENDSHAPE_NAMES: list[str] = [
    "_neutral",
    "browDownLeft",
    "browDownRight",
    "browInnerUp",
    "browOuterUpLeft",
    "browOuterUpRight",
    "cheekPuff",
    "cheekSquintLeft",
    "cheekSquintRight",
    "eyeBlinkLeft",
    "eyeBlinkRight",
    "eyeLookDownLeft",
    "eyeLookDownRight",
    "eyeLookInLeft",
    "eyeLookInRight",
    "eyeLookOutLeft",
    "eyeLookOutRight",
    "eyeLookUpLeft",
    "eyeLookUpRight",
    "eyeSquintLeft",
    "eyeSquintRight",
    "eyeWideLeft",
    "eyeWideRight",
    "jawForward",
    "jawLeft",
    "jawOpen",
    "jawRight",
    "mouthClose",
    "mouthDimpleLeft",
    "mouthDimpleRight",
    "mouthFrownLeft",
    "mouthFrownRight",
    "mouthFunnel",
    "mouthLeft",
    "mouthRight",
    "mouthLowerDownLeft",
    "mouthLowerDownRight",
    "mouthPressLeft",
    "mouthPressRight",
    "mouthPucker",
    "mouthRollLower",
    "mouthRollUpper",
    "mouthShrugLower",
    "mouthShrugUpper",
    "mouthSmileLeft",
    "mouthSmileRight",
    "mouthStretchLeft",
    "mouthStretchRight",
    "mouthUpperUpLeft",
    "mouthUpperUpRight",
    "noseSneerLeft",
    "noseSneerRight",
    "tongueOut",
]


@dataclass
class FaceExpression:
    """One detected face's bounding box and raw blendshape scores.

    Attributes
    ----------
    bbox_xyxy : tuple of float
        Detection bounding box, ``(x1, y1, x2, y2)`` pixels.
    confidence : float
        Detection confidence. Always ``1.0`` — see the constructor site's
        comment for why a constant is more honest here than a proxy metric.
    blendshape_scores : list of float
        Per-category activation, in ``[0, 1]``, parallel to
        :data:`BLENDSHAPE_NAMES`.
    """

    bbox_xyxy: tuple[float, float, float, float]
    confidence: float
    blendshape_scores: list[float]  # parallel to BLENDSHAPE_NAMES


class MediaPipeExpressionDetector:
    """MediaPipe Tasks ``FaceLandmarker``-based blendshape detector.

    Parameters
    ----------
    max_faces : int, default 5
        Maximum simultaneous faces to detect per frame.
    min_confidence : float, default 0.5
        Minimum face-detection/-presence confidence to keep a face.
    """

    def __init__(self, max_faces: int = 5, min_confidence: float = 0.5) -> None:
        import mediapipe as mp
        from mediapipe.tasks import python as mp_python
        from mediapipe.tasks.python import vision as mp_vision

        model_path = _ensure_download(_MODELS_DIR / "face_landmarker.task", _MEDIAPIPE_MODEL_URL)

        options = mp_vision.FaceLandmarkerOptions(
            base_options=mp_python.BaseOptions(model_asset_path=str(model_path)),
            running_mode=mp_vision.RunningMode.IMAGE,
            num_faces=max_faces,
            min_face_detection_confidence=min_confidence,
            min_face_presence_confidence=min_confidence,
            output_face_blendshapes=True,
            output_facial_transformation_matrixes=False,
        )
        self._mp = mp
        self._landmarker = mp_vision.FaceLandmarker.create_from_options(options)

    def detect(self, frame_bgr: np.ndarray) -> list[FaceExpression]:
        """Detect faces and their blendshape scores in one BGR frame.

        Parameters
        ----------
        frame_bgr : numpy.ndarray
            BGR frame, as returned by ``cv2.imread``/``cv2.VideoCapture``.

        Returns
        -------
        list of FaceExpression
            One entry per detected face.
        """
        h, w = frame_bgr.shape[:2]
        rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        mp_image = self._mp.Image(image_format=self._mp.ImageFormat.SRGB, data=rgb)
        result = self._landmarker.detect(mp_image)

        faces: list[FaceExpression] = []
        zipped = zip(result.face_landmarks, result.face_blendshapes, strict=False)
        for face_landmarks, categories in zipped:
            xs = [lm.x for lm in face_landmarks]
            ys = [lm.y for lm in face_landmarks]
            bbox = (min(xs) * w, min(ys) * h, max(xs) * w, max(ys) * h)
            faces.append(
                FaceExpression(
                    bbox_xyxy=bbox,
                    # FaceLandmarkerResult carries no per-face detection score —
                    # only per-category blendshape scores. min_face_detection_
                    # confidence already gates which faces are returned at all,
                    # so a constant here is more honest than inventing a proxy
                    # metric from the blendshape scores themselves.
                    confidence=1.0,
                    blendshape_scores=_ordered_scores(categories),
                )
            )
        return faces


def _ordered_scores(categories) -> list[float]:
    by_name = {c.category_name: c.score for c in categories}
    return [by_name.get(name, 0.0) for name in BLENDSHAPE_NAMES]


def crop_bbox(frame_bgr: np.ndarray, bbox_xyxy: tuple[float, float, float, float]) -> np.ndarray:
    """Crop a frame to a bounding box, clamped to the frame bounds.

    Parameters
    ----------
    frame_bgr : numpy.ndarray
        BGR frame to crop.
    bbox_xyxy : tuple of float
        ``(x1, y1, x2, y2)`` pixel coordinates, e.g. from
        :attr:`FaceExpression.bbox_xyxy`.

    Returns
    -------
    numpy.ndarray
        The cropped region. Possibly empty (shape ``(0, 0, 3)``) if the
        box is degenerate (e.g. fully outside the frame) — callers must
        check ``.size`` before use.

    Notes
    -----
    Used by the FER+ backend (:mod:`~expression.ferplus`), which needs a
    tight face crop rather than the full frame.
    """
    h, w = frame_bgr.shape[:2]
    x1, y1, x2, y2 = bbox_xyxy
    x1c, y1c = max(0, int(x1)), max(0, int(y1))
    x2c, y2c = min(w, int(x2)), min(h, int(y2))
    if x2c <= x1c or y2c <= y1c:
        return np.empty((0, 0, 3), dtype=frame_bgr.dtype)
    return frame_bgr[y1c:y2c, x1c:x2c]


def _ensure_download(dest: Path, url: str) -> Path:
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        print(f"[expression] Downloading {dest.name} …", flush=True)
        urllib.request.urlretrieve(url, dest)
        print(f"[expression] Downloaded to {dest}", flush=True)
    return dest
