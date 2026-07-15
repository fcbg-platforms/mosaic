"""Real-time single-frame pose estimation using MediaPipe Pose (Tasks API).

Compatible with mediapipe >= 0.10.  Requires pose_landmarker_lite.task
(or _full / _heavy) to be present next to this file.
"""
from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

import mediapipe as mp
import numpy as np
from mediapipe.tasks import python as _mp_python
from mediapipe.tasks.python import vision as _mp_vision

_LANDMARK_NAMES = [
    "NOSE", "LEFT_EYE_INNER", "LEFT_EYE", "LEFT_EYE_OUTER",
    "RIGHT_EYE_INNER", "RIGHT_EYE", "RIGHT_EYE_OUTER",
    "LEFT_EAR", "RIGHT_EAR", "MOUTH_LEFT", "MOUTH_RIGHT",
    "LEFT_SHOULDER", "RIGHT_SHOULDER", "LEFT_ELBOW", "RIGHT_ELBOW",
    "LEFT_WRIST", "RIGHT_WRIST", "LEFT_PINKY", "RIGHT_PINKY",
    "LEFT_INDEX", "RIGHT_INDEX", "LEFT_THUMB", "RIGHT_THUMB",
    "LEFT_HIP", "RIGHT_HIP", "LEFT_KNEE", "RIGHT_KNEE",
    "LEFT_ANKLE", "RIGHT_ANKLE", "LEFT_HEEL", "RIGHT_HEEL",
    "LEFT_FOOT_INDEX", "RIGHT_FOOT_INDEX",
]

_HERE = Path(__file__).parent


@dataclass
class Keypoint:
    x: float
    y: float
    z: float
    visibility: float
    name: str

    def to_dict(self) -> dict:
        return {"x": self.x, "y": self.y, "z": self.z,
                "visibility": self.visibility, "name": self.name}


class PoseEstimator:
    """Single-camera pose estimator using the MediaPipe 0.10+ Tasks API.

    Parameters
    ----------
    model_complexity:
        0 → lite model, 1 → full, 2 → heavy.
        Maps to the corresponding pose_landmarker_*.task file.
    min_detection_confidence / min_tracking_confidence:
        Passed to PoseLandmarkerOptions.
    model_path:
        Explicit path to a .task file.  If None, a file next to this
        module is chosen based on model_complexity.
    """

    _COMPLEXITY_NAMES = {0: "lite", 1: "full", 2: "heavy"}

    def __init__(
        self,
        model_complexity: int = 1,
        min_detection_confidence: float = 0.5,
        min_tracking_confidence: float = 0.5,
        model_path: str | None = None,
    ) -> None:
        if model_path is None:
            suffix = self._COMPLEXITY_NAMES.get(model_complexity, "lite")
            candidate = _HERE / f"pose_landmarker_{suffix}.task"
            # Fall back to lite if the requested complexity isn't downloaded.
            if not candidate.exists():
                candidate = _HERE / "pose_landmarker_lite.task"
            if not candidate.exists():
                raise FileNotFoundError(
                    f"No .task model found in {_HERE}. "
                    "Download pose_landmarker_lite.task from the MediaPipe models page."
                )
            model_path = str(candidate)

        options = _mp_vision.PoseLandmarkerOptions(
            base_options=_mp_python.BaseOptions(model_asset_path=model_path),
            running_mode=_mp_vision.RunningMode.IMAGE,
            num_poses=1,
            min_pose_detection_confidence=min_detection_confidence,
            min_pose_presence_confidence=min_detection_confidence,
            min_tracking_confidence=min_tracking_confidence,
        )
        self._landmarker = _mp_vision.PoseLandmarker.create_from_options(options)

    def estimate(self, frame_bgr: np.ndarray) -> list[Keypoint] | None:
        """Run pose estimation on a single BGR frame.

        Returns a list of 33 normalised landmarks or None if no person detected.
        """
        import cv2
        rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        result = self._landmarker.detect(mp_image)

        if not result.pose_landmarks:
            return None

        landmarks = result.pose_landmarks[0]
        return [
            Keypoint(
                x=lm.x, y=lm.y, z=lm.z,
                visibility=lm.visibility,
                name=_LANDMARK_NAMES[i] if i < len(_LANDMARK_NAMES) else str(i),
            )
            for i, lm in enumerate(landmarks)
        ]

    def close(self) -> None:
        self._landmarker.close()

    def __enter__(self) -> "PoseEstimator":
        return self

    def __exit__(self, *_) -> None:
        self.close()
