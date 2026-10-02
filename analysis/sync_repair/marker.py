"""
The visible mark on a frame a camera missed.

In an equal-count aligned video, a tick the camera did not answer still needs
a frame — the last real one is repeated so motion stays continuous and every
camera keeps the same length. Repeated silently, that frame is a lie: anyone
stepping through the video, or any plugin reading it, would take it for a
fresh image. So it carries a small red "MISSING" tag in its top-left corner.

Small and in a corner on purpose: analysis plugins (pose, gaze, faces) still
see an almost untouched image, while a person sees the gap at once.
"""

from __future__ import annotations

import cv2
import numpy as np

_RED = (40, 40, 220)  # BGR
_WHITE = (255, 255, 255)
LABEL = "MISSING"


def tag_geometry(width: int, height: int) -> tuple[int, int, float, int]:
    """(box_width, box_height, font_scale, thickness) for a frame size —
    scaled so the tag stays readable at 640x480 and modest at 1920x1080."""
    box_h = max(14, height // 18)
    scale = box_h / 30.0
    thickness = max(1, int(round(scale * 2)))
    (text_w, _), _ = cv2.getTextSize(LABEL, cv2.FONT_HERSHEY_SIMPLEX, scale, thickness)
    box_w = text_w + box_h // 2
    return box_w, box_h, scale, thickness


def mark_missing(frame: np.ndarray) -> np.ndarray:
    """A copy of `frame` with the MISSING tag drawn in the top-left corner.
    The input is not modified — the same decoded frame may be shown again,
    unmarked, as the real frame it is."""
    out = frame.copy()
    h, w = out.shape[:2]
    box_w, box_h, scale, thickness = tag_geometry(w, h)
    box_w = min(box_w, w)
    box_h = min(box_h, h)
    cv2.rectangle(out, (0, 0), (box_w - 1, box_h - 1), _RED, thickness=-1)
    baseline_y = int(box_h * 0.75)
    cv2.putText(
        out,
        LABEL,
        (box_h // 4, baseline_y),
        cv2.FONT_HERSHEY_SIMPLEX,
        scale,
        _WHITE,
        thickness,
        cv2.LINE_AA,
    )
    return out
