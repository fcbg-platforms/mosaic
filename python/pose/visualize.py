"""Draw MediaPipe Pose skeleton overlay on a BGR frame (for testing/offline use)."""
from __future__ import annotations

import cv2
import mediapipe as mp
import numpy as np

from pose.estimator import Keypoint

# MediaPipe bone connections — pairs of landmark indices
_CONNECTIONS: list[tuple[int, int]] = list(mp.solutions.pose.POSE_CONNECTIONS)


def draw_skeleton(
    frame_bgr: np.ndarray,
    keypoints: list[Keypoint],
    min_visibility: float = 0.4,
    bone_color: tuple[int, int, int] = (60, 220, 90),
    joint_color: tuple[int, int, int] = (255, 160, 0),
    bone_thickness: int = 2,
    joint_radius: int = 4,
) -> np.ndarray:
    """Return a copy of *frame_bgr* with the pose skeleton drawn on it.

    Parameters
    ----------
    keypoints:
        List of 33 Keypoint objects (normalised coordinates [0,1]).
    min_visibility:
        Landmarks with visibility below this threshold are skipped.
    """
    h, w = frame_bgr.shape[:2]
    out = frame_bgr.copy()

    pts = [(int(kp.x * w), int(kp.y * h)) for kp in keypoints]
    vis = [kp.visibility for kp in keypoints]

    # Bones
    for i, j in _CONNECTIONS:
        if i >= len(pts) or j >= len(pts):
            continue
        if vis[i] < min_visibility or vis[j] < min_visibility:
            continue
        cv2.line(out, pts[i], pts[j], bone_color, bone_thickness, cv2.LINE_AA)

    # Joints
    for idx, (px, py) in enumerate(pts):
        if idx >= len(vis) or vis[idx] < min_visibility:
            continue
        cv2.circle(out, (px, py), joint_radius, joint_color, -1, cv2.LINE_AA)

    return out
