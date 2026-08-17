"""
YOLO26-depth backend for monocular depth estimation.

Recommended model weights by speed/accuracy trade-off:
  yolo26n-depth.pt — nano,   fastest
  yolo26s-depth.pt — small,  balanced
  yolo26m-depth.pt — medium, most accurate

Produces a dense per-pixel depth map per frame (meters), a completely
different output shape from pose estimation's keypoints/skeleton — so this
gets its own estimator class rather than extending HumanPoseEstimator/
PoseResult, and run_pose.py writes its output as a colorized video into a
session's depth/ folder instead of a .pose.json sidecar (see
run_depth_session()/run_depth_video() below).

Install: pip install ultralytics (same package as the pose backend — the
depth task ships in the same YOLO() API surface, just a different
checkpoint/task type).
"""

from __future__ import annotations

import cv2
import numpy as np

try:
    from ultralytics import YOLO as _YOLO

    _ULTRALYTICS_OK = True
except ImportError:
    _ULTRALYTICS_OK = False
    _YOLO = None  # type: ignore[assignment]


class DepthEstimator:
    """Wraps a YOLO26-depth model for single-frame monocular depth inference.

    Parameters
    ----------
    model_name : str, default "yolo26n-depth.pt"
        YOLO26 depth model variant. Downloaded automatically on first use.
    device : str or None, default None
        Inference device — "cpu" / "cuda:0" / "mps". None auto-detects
        (prefers CUDA -> MPS -> CPU), same convention as HumanPoseEstimator.

    Raises
    ------
    ImportError
        If ``ultralytics`` is not installed.
    """

    def __init__(self, model_name: str = "yolo26n-depth.pt", device: str | None = None) -> None:
        if not _ULTRALYTICS_OK:
            raise ImportError("ultralytics is not installed.  Run: pip install ultralytics")

        self._device = device or self._auto_device()
        print(f"[DepthEstimator] Loading {model_name} on {self._device} …", flush=True)
        self._model = _YOLO(model_name)
        # Warm-up pass (avoids slow first real inference), same as HumanPoseEstimator.
        dummy = np.zeros((320, 320, 3), dtype=np.uint8)
        self._model(dummy, verbose=False, device=self._device)
        print("[DepthEstimator] Ready.", flush=True)

    def infer_colorized(self, frame: np.ndarray) -> np.ndarray:
        """Runs depth inference on one BGR frame and returns a colorized BGR
        visualization, resized to the input frame's own dimensions (the
        model's internal output resolution can differ from the source).

        Colour is scaled per-frame (this frame's own min/max depth), not
        against a fixed global range — absolute distances vary a lot by
        scene, and a fixed range would wash out most sessions either too
        dark or too bright. This means colour is only comparable *within*
        one frame, not across frames or videos — a deliberate simplification
        for a quick visualization output, not a calibrated depth export.
        """
        results = self._model(frame, device=self._device, verbose=False)
        depth = results[0].depth.data.cpu().numpy()  # (H, W) float32, meters

        d_min, d_max = float(depth.min()), float(depth.max())
        if d_max > d_min:
            normalized = ((depth - d_min) / (d_max - d_min) * 255.0).astype(np.uint8)
        else:
            normalized = np.zeros_like(depth, dtype=np.uint8)

        colorized = cv2.applyColorMap(normalized, cv2.COLORMAP_INFERNO)
        if colorized.shape[:2] != frame.shape[:2]:
            colorized = cv2.resize(colorized, (frame.shape[1], frame.shape[0]))
        return colorized

    @staticmethod
    def _auto_device() -> str:
        try:
            import torch

            if torch.cuda.is_available():
                return "cuda:0"
            if torch.backends.mps.is_available():
                return "mps"
        except ImportError:
            pass
        return "cpu"
