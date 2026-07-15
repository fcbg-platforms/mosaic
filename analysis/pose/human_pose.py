"""
YOLOv8-pose backend for real-time human pose estimation.

Recommended model weights by speed/accuracy trade-off:
  yolov8n-pose.pt  — nano,   ~4 MB,  fastest,  good for CPU preview (≥15 fps)
  yolov8s-pose.pt  — small,  ~24 MB, balanced  (CPU ≥8 fps, GPU ≥60 fps)
  yolov8m-pose.pt  — medium, ~52 MB, accurate  (GPU recommended)
  yolov8l-pose.pt  — large,  ~87 MB, best      (GPU required)

Install: pip install ultralytics
"""
from __future__ import annotations

import time
from pathlib import Path
from typing import List, Optional

import numpy as np

from .keypoints import PoseResult, SubjectPose

# ── lazy import: ultralytics is optional ─────────────────────────────────────
try:
    from ultralytics import YOLO as _YOLO
    _ULTRALYTICS_OK = True
except ImportError:
    _ULTRALYTICS_OK = False
    _YOLO = None  # type: ignore[assignment]


class HumanPoseEstimator:
    """
    Wraps YOLOv8-pose for single-frame inference.

    Parameters
    ----------
    model_name:
        YOLOv8 model variant (e.g. ``"yolov8n-pose.pt"``).  Downloaded
        automatically from Ultralytics CDN on first use (~4–87 MB).
    device:
        Inference device — ``"cpu"``, ``"cuda:0"``, ``"mps"`` (Apple Silicon).
        Pass ``None`` to auto-detect (prefers CUDA → MPS → CPU).
    conf_threshold:
        Minimum detection confidence to include a subject.
    iou_threshold:
        NMS IoU threshold.
    """

    def __init__(
        self,
        model_name: str = "yolov8n-pose.pt",
        device: Optional[str] = None,
        conf_threshold: float = 0.40,
        iou_threshold: float  = 0.70,
    ) -> None:
        if not _ULTRALYTICS_OK:
            raise ImportError(
                "ultralytics is not installed.  Run: pip install ultralytics"
            )

        self._conf = conf_threshold
        self._iou  = iou_threshold
        self._device = device or self._auto_device()

        print(f"[HumanPoseEstimator] Loading {model_name} on {self._device} …", flush=True)
        self._model = _YOLO(model_name)
        # Warm-up pass (avoids slow first real inference)
        dummy = np.zeros((320, 320, 3), dtype=np.uint8)
        self._model(dummy, verbose=False, device=self._device)
        print("[HumanPoseEstimator] Ready.", flush=True)

    # ── Public API ────────────────────────────────────────────────────────────

    def infer(
        self,
        frame: np.ndarray,
        frame_index: int = 0,
        timestamp_ns: int = 0,
        camera_index: int = 0,
    ) -> PoseResult:
        """
        Run pose estimation on one BGR frame (as returned by cv2.imread / VideoCapture).

        Returns
        -------
        PoseResult
            Structured result containing per-subject keypoints.
        """
        t0 = time.perf_counter()
        results = self._model(
            frame,
            conf=self._conf,
            iou=self._iou,
            device=self._device,
            verbose=False,
        )
        inference_ms = (time.perf_counter() - t0) * 1000.0

        subjects: List[SubjectPose] = []
        for res in results:
            if res.keypoints is None:
                continue
            kpts_xy   = res.keypoints.xy.cpu().numpy()    # (N, 17, 2)
            kpts_conf = res.keypoints.conf                 # may be None
            boxes     = res.boxes

            for i, kxy in enumerate(kpts_xy):
                vis: List[float]
                if kpts_conf is not None:
                    vis = kpts_conf[i].cpu().numpy().tolist()
                else:
                    vis = [1.0] * kxy.shape[0]

                det_conf = float(boxes.conf[i].cpu()) if boxes is not None else 1.0
                bbox = (0.0, 0.0, 0.0, 0.0)
                if boxes is not None:
                    b = boxes.xyxy[i].cpu().numpy()
                    bbox = (float(b[0]), float(b[1]), float(b[2]), float(b[3]))

                subjects.append(SubjectPose(
                    subject_id=i,
                    confidence=det_conf,
                    keypoints=[(float(pt[0]), float(pt[1])) for pt in kxy],
                    visibilities=vis,
                    bbox_xyxy=bbox,
                ))

        return PoseResult(
            frame_index=frame_index,
            timestamp_ns=timestamp_ns,
            camera_index=camera_index,
            subjects=subjects,
            backend=f"yolov8-pose/{self._model.ckpt_path}",
            inference_ms=inference_ms,
        )

    # ── Helpers ───────────────────────────────────────────────────────────────

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
