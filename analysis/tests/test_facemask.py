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

from facemask.masking import expand_and_clip


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
