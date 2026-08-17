"""
Pure geometry/pixel helpers for face anonymization — kept dependency-free
(only numpy/opencv) and side-effect-free so they're cheap to unit test.
"""

from __future__ import annotations

import cv2
import numpy as np

Box = tuple[float, float, float, float]  # x1, y1, x2, y2 (pixel coords)


def expand_and_clip(box: Box, margin_frac: float, frame_w: int, frame_h: int) -> Box:
    """Pad a face box and clip it to the frame bounds.

    Pads ``box`` by ``margin_frac`` of its own width/height (so hairline,
    ears, and chin are covered, not just the tight landmark/detector box),
    then clips the result to ``[0, frame_w] x [0, frame_h]``.

    Parameters
    ----------
    box : Box
        ``(x1, y1, x2, y2)`` pixel coordinates of the detected face box.
    margin_frac : float
        Fraction of the box's own width/height to pad on each side
        (e.g. ``0.25`` pads a 100 px-wide box by 25 px per side).
    frame_w, frame_h : int
        Frame dimensions in pixels, used as the clip bounds.

    Returns
    -------
    Box
        The padded, clipped ``(x1, y1, x2, y2)`` box. Always a valid,
        non-negative box even when the input box touches the frame edge.

    Notes
    -----
    Padding is applied before clipping, so a face near the frame edge is
    padded first and only then clamped — it cannot produce a negative or
    out-of-bounds coordinate a caller could mis-slice. See
    :doc:`/math/face_masking` for the exact padding/clamp formula.
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
    """Blur or solid-fill every box's region in ``frame``, in place.

    Parameters
    ----------
    frame : numpy.ndarray
        BGR frame, modified in place.
    boxes : list of Box
        Regions to mask, e.g. from :func:`expand_and_clip`. A degenerate
        box (``x2<=x1`` or ``y2<=y1``) is silently skipped.
    style : {"blur", "box"}
        ``"blur"`` applies a Gaussian blur with its kernel size scaled to
        the box's own size; ``"box"`` fills the region with a solid color.

    Returns
    -------
    numpy.ndarray
        ``frame``, returned for convenient call-site chaining (it was
        already modified in place).

    Notes
    -----
    See :doc:`/math/face_masking` for the blur-kernel sizing formula.
    """
    for x1, y1, x2, y2 in boxes:
        ix1, iy1 = int(round(x1)), int(round(y1))
        ix2, iy2 = int(round(x2)), int(round(y2))
        if ix2 <= ix1 or iy2 <= iy1:
            continue

        if style == "box":
            cv2.rectangle(frame, (ix1, iy1), (ix2, iy2), (20, 20, 20), thickness=-1)
            continue

        region_w, region_h = ix2 - ix1, iy2 - iy1
        # A region clipped to a thin sliver near the frame edge (a real,
        # observed case — expand_and_clip() clamps to frame bounds, so an
        # edge-adjacent detection can legitimately produce one) can be
        # smaller than a size-proportional kernel would need — cv2.GaussianBlur
        # throws if ksize exceeds the region's own dimensions. Fall back to
        # the same solid fill as the "box" style rather than risk leaving a
        # face unmasked or crashing the whole run.
        if min(region_w, region_h) < 3:
            cv2.rectangle(frame, (ix1, iy1), (ix2, iy2), (20, 20, 20), thickness=-1)
            continue

        def _largest_odd_leq(n: int) -> int:
            return n if n % 2 == 1 else n - 1

        # Kernel scaled to region size (must be odd and >= 3) so small/distant
        # faces and large/close faces both get proportionally strong blur —
        # then clamped to fit both of the region's actual dimensions.
        k = max(3, (min(region_w, region_h) // 3) | 1)
        k = min(k, _largest_odd_leq(region_w), _largest_odd_leq(region_h))
        region = frame[iy1:iy2, ix1:ix2]
        frame[iy1:iy2, ix1:ix2] = cv2.GaussianBlur(region, (k, k), 0)

    return frame
