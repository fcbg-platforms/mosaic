"""
Pure-logic tests for facemask/masking.py — no video/model fixtures needed.

expand_and_clip() is the highest-value function to get right in this feature:
a silent bug here (wrong clip order, off-by-one on the margin) means a face
that should have been anonymized isn't fully covered — a privacy leak, not
just a cosmetic bug.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

import numpy as np

from facemask.masking import apply_mask, expand_and_clip


def test_expand_and_clip_centered_box_pads_by_margin():
    # 100x100 box in a large frame: 25% margin pads 25px each side.
    box = (100.0, 100.0, 200.0, 200.0)
    result = expand_and_clip(box, margin_frac=0.25, frame_w=1000, frame_h=1000)
    assert result == (75.0, 75.0, 225.0, 225.0)


def test_expand_and_clip_clips_to_frame_bounds():
    # Box near the top-left corner: padding would go negative without clipping.
    box = (0.0, 0.0, 40.0, 40.0)
    result = expand_and_clip(box, margin_frac=0.5, frame_w=1000, frame_h=1000)
    x1, y1, x2, y2 = result
    assert x1 == 0.0
    assert y1 == 0.0
    assert x2 == 60.0
    assert y2 == 60.0


def test_expand_and_clip_clips_bottom_right_edge():
    box = (960.0, 960.0, 1000.0, 1000.0)
    result = expand_and_clip(box, margin_frac=0.5, frame_w=1000, frame_h=1000)
    x1, y1, x2, y2 = result
    assert x2 == 1000.0
    assert y2 == 1000.0
    assert x1 == 940.0
    assert y1 == 940.0


def test_expand_and_clip_zero_margin_is_identity():
    box = (10.0, 20.0, 30.0, 40.0)
    result = expand_and_clip(box, margin_frac=0.0, frame_w=1000, frame_h=1000)
    assert result == box


def test_expand_and_clip_degenerate_zero_size_box():
    box = (50.0, 50.0, 50.0, 50.0)
    result = expand_and_clip(box, margin_frac=0.25, frame_w=1000, frame_h=1000)
    assert result == (50.0, 50.0, 50.0, 50.0)


def test_apply_mask_blur_on_thin_edge_sliver_does_not_raise():
    # A face box clipped to a thin sliver by expand_and_clip() near a frame
    # edge — this used to crash cv2.GaussianBlur when its size-proportional
    # kernel exceeded the region's own (tiny) dimensions.
    frame = np.zeros((480, 640, 3), dtype=np.uint8)
    box = (0.0, 0.0, 40.0, 2.0)  # 40px wide, only 2px tall
    apply_mask(frame, [box], style="blur")  # must not raise


def test_apply_mask_blur_on_tiny_region_falls_back_to_solid_fill():
    frame = np.full((480, 640, 3), 200, dtype=np.uint8)
    box = (10.0, 10.0, 11.0, 11.0)  # 1x1px region — too small to blur
    apply_mask(frame, [box], style="blur")
    assert (frame[10:11, 10:11] == (20, 20, 20)).all()


def test_apply_mask_blur_on_normal_region_still_blurs():
    frame = np.zeros((480, 640, 3), dtype=np.uint8)
    # A sharp bright square INSIDE the masked region (not just the whole
    # region uniformly bright) so blurring has a visible internal edge to
    # soften — a uniform region has no internal edge and would look
    # unchanged after a blur regardless of whether blurring ran at all.
    frame[140:160, 140:160] = 255
    box = (100.0, 100.0, 200.0, 200.0)
    apply_mask(frame, [box], style="blur")
    region = frame[100:200, 100:200]
    # A real blur bleeds the bright square's edge into its neighbors —
    # not every pixel stays at the original hard 0/255 split.
    assert not np.array_equal(np.unique(region), [0, 255])
