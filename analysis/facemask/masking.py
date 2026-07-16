"""
Pure geometry/pixel helpers for face anonymization — kept dependency-free
(only numpy/opencv) and side-effect-free so they're cheap to unit test.
"""
from __future__ import annotations

import cv2
import numpy as np

Box = tuple[float, float, float, float]  # x1, y1, x2, y2 (pixel coords)


def expand_and_clip(box: Box, margin_frac: float, frame_w: int, frame_h: int) -> Box:
    """
    Pad a detected face box by ``margin_frac`` of its own width/height (so
    hairline/ears/chin are covered, not just the tight landmark/detector box),
    then clip to the frame bounds.

    A face right at the frame edge must still clip to a valid, non-negative
    box rather than producing an out-of-bounds region a caller could
    mis-slice.
    """
    x1, y1, x2, y2 = box
    w = max(0.0, x2 - x1)
    h = max(0.0, y2 - y1)
    mx = w * margin_frac
    my = h * margin_frac

    x1 = max(0.0, x1 - mx)
    y1 = max(0.0, y1 - my)
    x2 = min(float(frame_w), x2 + mx)
    y2 = min(float(frame_h), y2 + my)

    return (x1, y1, x2, y2)


def apply_mask(frame: np.ndarray, boxes: list[Box], style: str) -> np.ndarray:
    """
    Blur or solid-fill every box's region in ``frame`` in place, and return it.

    ``style``: "blur" (Gaussian, kernel scaled to box size) or "box" (solid fill).
    """
    for (x1, y1, x2, y2) in boxes:
        ix1, iy1 = int(round(x1)), int(round(y1))
        ix2, iy2 = int(round(x2)), int(round(y2))
        if ix2 <= ix1 or iy2 <= iy1:
            continue

        if style == "box":
            cv2.rectangle(frame, (ix1, iy1), (ix2, iy2), (20, 20, 20), thickness=-1)
            continue

        region = frame[iy1:iy2, ix1:ix2]
        # Kernel scaled to region size (must be odd and >= 3) so small/distant
        # faces and large/close faces both get proportionally strong blur.
        k = max(3, (min(ix2 - ix1, iy2 - iy1) // 3) | 1)
        frame[iy1:iy2, ix1:ix2] = cv2.GaussianBlur(region, (k, k), 0)

    return frame
