"""
Pure geometry/pixel helpers for face anonymization — kept dependency-free
(only numpy/opencv) and side-effect-free so they're cheap to unit test.
"""

from __future__ import annotations

import cv2
import numpy as np

Box = tuple[float, float, float, float]  # x1, y1, x2, y2 (pixel coords)


def _largest_odd_leq(n: int) -> int:
    """Largest odd integer <= n. cv2's Gaussian kernels must be odd."""
    return n if n % 2 == 1 else n - 1


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

        # Kernel scaled to region size (must be odd and >= 3) so small/distant
        # faces and large/close faces both get proportionally strong blur —
        # then clamped to fit both of the region's actual dimensions.
        k = max(3, (min(region_w, region_h) // 3) | 1)
        k = min(k, _largest_odd_leq(region_w), _largest_odd_leq(region_h))
        region = frame[iy1:iy2, ix1:ix2]
        frame[iy1:iy2, ix1:ix2] = cv2.GaussianBlur(region, (k, k), 0)

    return frame


# ── Whole-region masking ───────────────────────────────────────────────────
#
# The helpers below mask an arbitrary boolean region rather than a rectangle,
# for the whole-body anonymization mode. apply_mask() above is deliberately
# left untouched: it is the face path, it is what docs/math/face_masking.rst
# documents, and its rectangle slicing cannot express a silhouette.


def region_blur_kernel(frame_w: int, frame_h: int) -> int:
    """Gaussian kernel size for a whole-region blur, scaled to the frame.

    Deliberately *not* scaled to the masked region the way :func:`apply_mask`
    scales to each face box. A person silhouette's smallest dimension is
    roughly the width of an ankle, so a region-scaled kernel would collapse to
    near-nothing for a perfectly ordinary standing person. What has to be
    defeated here is recognisability at the output resolution, which is a
    property of the frame, not of how much of it one person happens to fill.

    Returns an odd value >= 3, clamped so it can never exceed either frame
    dimension (``cv2.GaussianBlur`` rejects a kernel larger than the image).
    """
    k = max(3, (min(frame_w, frame_h) // 20) | 1)
    k = min(k, _largest_odd_leq(frame_w), _largest_odd_leq(frame_h))
    return max(3, k)


def dilate_mask(mask: np.ndarray, radius_px: int) -> np.ndarray:
    """Grow ``mask`` outward by ``radius_px``, the whole-body analogue of
    :func:`expand_and_clip`'s box padding.

    A segmentation mask hugs the silhouette, so without slack the outermost
    ring of person pixels — hair, shoulders, fingers — stays unblurred. The
    caller picks the radius; see run_face_mask.py for why it is absolute
    rather than proportional to the person's own size.

    Uses a rectangular structuring element: OpenCV optimises it as two
    separable passes (an elliptical one is several times slower at this
    radius), and its corners over-reach by a factor of √2, which errs toward
    covering more rather than less.

    ``radius_px <= 0`` returns the mask unchanged. The result is always a
    superset of the input — dilation may only ever add coverage.
    """
    if radius_px <= 0 or not mask.any():
        return mask
    size = 2 * int(radius_px) + 1
    kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (size, size))
    return cv2.dilate(mask.astype(np.uint8), kernel).astype(bool)


def rasterize_boxes(mask: np.ndarray, boxes: list[Box]) -> np.ndarray:
    """OR every box in ``boxes`` into ``mask``, in place.

    Used to union face-detector boxes with a person-segmentation mask, so a
    person the segmenter missed still has their face covered.

    Coordinates round *outward* (floor the top-left, ceil the bottom-right),
    unlike :func:`apply_mask`'s ``int(round())``. Rounding to nearest can shave
    up to half a pixel off each edge; for a box already padded by 25% that is
    irrelevant, but here it is the only guard the union adds over the
    segmentation mask, so it errs toward covering more. Do not "fix" this to
    match apply_mask.

    Degenerate boxes (zero or negative extent) are skipped, mirroring
    :func:`apply_mask`.
    """
    h, w = mask.shape[:2]
    for x1, y1, x2, y2 in boxes:
        ix1 = max(0, int(np.floor(x1)))
        iy1 = max(0, int(np.floor(y1)))
        ix2 = min(w, int(np.ceil(x2)))
        iy2 = min(h, int(np.ceil(y2)))
        if ix2 <= ix1 or iy2 <= iy1:
            continue
        mask[iy1:iy2, ix1:ix2] = True
    return mask


def apply_mask_region(frame: np.ndarray, mask: np.ndarray, style: str) -> np.ndarray:
    """Blur or solid-fill every pixel where ``mask`` is True, in place.

    ``mask`` must be ``(H, W)`` boolean and match ``frame``'s own height and
    width. A mismatch raises rather than letting numpy broadcast something
    plausible — a silently misaligned mask is exactly the failure this whole
    feature has to avoid.

    The blur runs over the mask's bounding box rather than the whole frame,
    which is several times cheaper for a typical person, and the crop is padded
    by half the kernel so every masked pixel's kernel footprint is real image
    data rather than OpenCV's reflected border. That padding is what makes the
    cropped result identical to a whole-frame blur, not merely similar.
    """
    if mask.shape[:2] != frame.shape[:2]:
        raise ValueError(f"mask shape {mask.shape[:2]} != frame shape {frame.shape[:2]}")
    if not mask.any():
        return frame

    if style == "box":
        frame[mask] = (20, 20, 20)
        return frame

    frame_h, frame_w = frame.shape[:2]
    if min(frame_w, frame_h) < 3:
        # Too small for any valid kernel — same solid-fill fallback
        # apply_mask() uses for a sliver region.
        frame[mask] = (20, 20, 20)
        return frame

    k = region_blur_kernel(frame_w, frame_h)

    rows = np.flatnonzero(mask.any(axis=1))
    cols = np.flatnonzero(mask.any(axis=0))
    pad = k // 2
    y1 = max(0, int(rows[0]) - pad)
    y2 = min(frame_h, int(rows[-1]) + 1 + pad)
    x1 = max(0, int(cols[0]) - pad)
    x2 = min(frame_w, int(cols[-1]) + 1 + pad)

    roi = frame[y1:y2, x1:x2]
    blurred = cv2.GaussianBlur(roi, (k, k), 0)
    np.copyto(roi, blurred, where=mask[y1:y2, x1:x2, None])
    return frame
