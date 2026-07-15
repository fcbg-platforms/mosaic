"""Real-time gaze estimation using MediaPipe FaceLandmarker (Tasks API).

Uses the same Tasks API as pose_estimator.py (mediapipe >= 0.10).
Requires face_landmarker.task next to this file — downloaded automatically
on first run if not present.

The standard face_landmarker model outputs 478 landmarks:
  0-467  : face mesh base
  468-472: left iris  (center + 4 cardinal points)
  473-477: right iris (center + 4 cardinal points)

Gaze direction is estimated from the iris-centre offset relative to the
eye-socket centre, normalised by the inter-canthus width of that eye.
"""
from __future__ import annotations

import os
import urllib.request
from dataclasses import dataclass
from pathlib import Path

import cv2
import mediapipe as mp
import numpy as np
from mediapipe.tasks import python as _mp_python
from mediapipe.tasks.python import vision as _mp_vision

# ── Landmark indices ───────────────────────────────────────────────────────
_LEFT_IRIS_CENTER  = 468
_RIGHT_IRIS_CENTER = 473

_LEFT_EYE_OUTER   = 33
_LEFT_EYE_INNER   = 133
_LEFT_EYE_TOP     = 159
_LEFT_EYE_BOTTOM  = 145

_RIGHT_EYE_INNER  = 362
_RIGHT_EYE_OUTER  = 263
_RIGHT_EYE_TOP    = 386
_RIGHT_EYE_BOTTOM = 374

_HERE       = Path(__file__).parent
_MODEL_FILE = _HERE / "face_landmarker.task"
_MODEL_URL  = (
    "https://storage.googleapis.com/mediapipe-models/"
    "face_landmarker/face_landmarker/float16/1/face_landmarker.task"
)


def _ensure_model() -> str:
    if not _MODEL_FILE.exists():
        print(f"[gaze] Downloading face_landmarker.task …", flush=True)
        urllib.request.urlretrieve(_MODEL_URL, _MODEL_FILE)
        print(f"[gaze] Downloaded to {_MODEL_FILE}", flush=True)
    return str(_MODEL_FILE)


@dataclass
class GazeResult:
    face_box:   tuple[float, float, float, float]  # x, y, w, h  (0-1 normalised)
    left_iris:  tuple[float, float]               # normalised x, y
    right_iris: tuple[float, float]
    gaze_dx:    float                             # -1 = left, +1 = right
    gaze_dy:    float                             # -1 = up,   +1 = down

    def to_dict(self) -> dict:
        return {
            "face_box":   {"x": self.face_box[0], "y": self.face_box[1],
                           "w": self.face_box[2], "h": self.face_box[3]},
            "left_iris":  {"x": self.left_iris[0],  "y": self.left_iris[1]},
            "right_iris": {"x": self.right_iris[0], "y": self.right_iris[1]},
            "gaze_dx":    self.gaze_dx,
            "gaze_dy":    self.gaze_dy,
        }


class GazeEstimator:
    """Single-camera gaze estimator.  Thread-unsafe — one instance per subprocess."""

    def __init__(self) -> None:
        model_path = _ensure_model()
        options = _mp_vision.FaceLandmarkerOptions(
            base_options=_mp_python.BaseOptions(model_asset_path=model_path),
            running_mode=_mp_vision.RunningMode.IMAGE,
            num_faces=1,
            min_face_detection_confidence=0.3,
            min_face_presence_confidence=0.3,
            min_tracking_confidence=0.3,
            output_face_blendshapes=False,
            output_facial_transformation_matrixes=False,
        )
        self._landmarker = _mp_vision.FaceLandmarker.create_from_options(options)

    def estimate(self, frame_bgr: np.ndarray) -> GazeResult | None:
        """Run gaze estimation on one BGR frame.  Returns GazeResult or None."""
        rgb      = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        result   = self._landmarker.detect(mp_image)

        if not result.face_landmarks:
            return None

        lm = result.face_landmarks[0]
        if len(lm) < 478:
            return None  # iris landmarks not present in this model

        # ── Face bounding box from first 468 base landmarks ────────────────
        xs = [lm[i].x for i in range(468)]
        ys = [lm[i].y for i in range(468)]
        fx, fy = min(xs), min(ys)
        fw, fh = max(xs) - fx, max(ys) - fy

        # ── Iris centres ───────────────────────────────────────────────────
        li_x, li_y = lm[_LEFT_IRIS_CENTER].x,  lm[_LEFT_IRIS_CENTER].y
        ri_x, ri_y = lm[_RIGHT_IRIS_CENTER].x, lm[_RIGHT_IRIS_CENTER].y

        # ── Eye-socket geometry for normalisation ──────────────────────────
        left_cx  = (lm[_LEFT_EYE_OUTER].x  + lm[_LEFT_EYE_INNER].x)  / 2
        left_cy  = (lm[_LEFT_EYE_TOP].y    + lm[_LEFT_EYE_BOTTOM].y) / 2
        left_w   = abs(lm[_LEFT_EYE_OUTER].x - lm[_LEFT_EYE_INNER].x)

        right_cx = (lm[_RIGHT_EYE_INNER].x + lm[_RIGHT_EYE_OUTER].x) / 2
        right_cy = (lm[_RIGHT_EYE_TOP].y   + lm[_RIGHT_EYE_BOTTOM].y) / 2
        right_w  = abs(lm[_RIGHT_EYE_INNER].x - lm[_RIGHT_EYE_OUTER].x)

        eye_w = (left_w + right_w) / 2
        if eye_w < 1e-6:
            return None

        # ── Gaze offset: iris vs socket centre, normalised by eye width ────
        dx = ((li_x - left_cx) / eye_w + (ri_x - right_cx) / eye_w) / 2
        dy = ((li_y - left_cy) / eye_w + (ri_y - right_cy) / eye_w) / 2

        dx = float(max(-1.0, min(1.0, dx)))
        dy = float(max(-1.0, min(1.0, dy)))

        return GazeResult(
            face_box=(float(fx), float(fy), float(fw), float(fh)),
            left_iris=(float(li_x), float(li_y)),
            right_iris=(float(ri_x), float(ri_y)),
            gaze_dx=dx,
            gaze_dy=dy,
        )

    def close(self) -> None:
        self._landmarker.close()

    def __enter__(self) -> "GazeEstimator":
        return self

    def __exit__(self, *_) -> None:
        self.close()
