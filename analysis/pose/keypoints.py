"""Keypoint skeleton definitions for COCO (human) and common animal models."""
from __future__ import annotations
from dataclasses import dataclass, field
from typing import List, Tuple

# ── COCO 17-keypoint skeleton (YOLOv8-pose default) ──────────────────────────
COCO_KEYPOINTS: List[str] = [
    "nose",
    "left_eye",  "right_eye",
    "left_ear",  "right_ear",
    "left_shoulder", "right_shoulder",
    "left_elbow",    "right_elbow",
    "left_wrist",    "right_wrist",
    "left_hip",      "right_hip",
    "left_knee",     "right_knee",
    "left_ankle",    "right_ankle",
]

# Skeleton edges as (kp_index_a, kp_index_b) pairs for drawing limbs
COCO_SKELETON: List[Tuple[int, int]] = [
    (0, 1), (0, 2), (1, 3), (2, 4),           # face
    (5, 6),                                     # shoulders
    (5, 7), (7, 9), (6, 8), (8, 10),           # arms
    (5, 11), (6, 12), (11, 12),                # torso
    (11, 13), (13, 15), (12, 14), (14, 16),    # legs
]

COCO_COLORS: List[Tuple[int, int, int]] = [
    (255, 0,   85), (255, 0,   0),  (255, 85,  0),
    (255, 170, 0),  (255, 255, 0),  (170, 255, 0),
    (85,  255, 0),  (0,   255, 0),  (0,   255, 85),
    (0,   255, 170),(0,   255, 255),(0,   170, 255),
    (0,   85,  255),(0,   0,   255),(85,  0,   255),
    (170, 0,   255),(255, 0,   170),
]

# ── DeepLabCut-style mouse skeleton (example — replace with your DLC project) ─
MOUSE_KEYPOINTS: List[str] = [
    "snout", "left_ear", "right_ear", "neck",
    "left_forepaw", "right_forepaw",
    "mid_back",
    "left_hindpaw", "right_hindpaw",
    "tail_base", "tail_mid", "tail_tip",
]

MOUSE_SKELETON: List[Tuple[int, int]] = [
    (0, 1), (0, 2), (0, 3),            # head
    (3, 4), (3, 5),                     # forepaws
    (3, 6), (6, 7), (6, 8),            # mid-body + hindpaws
    (6, 9), (9, 10), (10, 11),         # tail
]


@dataclass
class PoseResult:
    """Normalised output from any pose estimator backend.

    Attributes
    ----------
    frame_index : int
        Caller-supplied frame index this result was computed from.
    timestamp_ns : int
        Caller-supplied timestamp (ns) this result was computed from.
    camera_index : int
        Caller-supplied camera index this result was computed from.
    subjects : list of SubjectPose
        One entry per detected subject in this frame.
    backend : str, default "unknown"
        Identifies which estimator/model produced this result (e.g.
        ``"yolov8-pose/yolov8n-pose.pt"``).
    inference_ms : float, default 0.0
        Wall-clock inference time for this frame, in milliseconds.
    """
    frame_index: int
    timestamp_ns: int
    camera_index: int
    subjects: List["SubjectPose"] = field(default_factory=list)
    backend: str = "unknown"
    inference_ms: float = 0.0


@dataclass
class SubjectPose:
    """Keypoints for a single detected subject.

    Attributes
    ----------
    subject_id : int
        Tracking ID, or ``-1`` if no cross-frame tracking is performed.
    confidence : float
        Overall detection confidence, in ``[0, 1]``.
    keypoints : list of tuple of float
        ``(x_px, y_px)`` per keypoint, in the backend's own keypoint order
        (e.g. :data:`COCO_KEYPOINTS`).
    visibilities : list of float
        Per-keypoint confidence, in ``[0, 1]``, parallel to ``keypoints``.
    bbox_xyxy : tuple of float, default (0, 0, 0, 0)
        Subject's detection bounding box, ``(x1, y1, x2, y2)`` pixels.
    """
    subject_id: int                          # tracking ID (-1 if no tracking)
    confidence: float                        # overall detection confidence 0-1
    keypoints: List[Tuple[float, float]]     # (x_px, y_px) per keypoint
    visibilities: List[float]                # 0-1 confidence per keypoint
    bbox_xyxy: Tuple[float, float, float, float] = (0, 0, 0, 0)
