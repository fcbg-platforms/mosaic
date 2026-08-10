"""Pure-logic tests for transcribe/windowing.py's confirm_segments() — no
numpy/scipy/torch/faster-whisper import required."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from transcribe.windowing import Segment, confirm_segments, trim_buffer_samples


def test_segment_well_before_margin_is_confirmed():
    segs = [Segment(0.0, 2.0, "hello there")]
    confirmed, tentative, watermark = confirm_segments(segs, buffer_duration_sec=5.0,
                                                         trailing_margin_sec=1.0)
    assert confirmed == [Segment(0.0, 2.0, "hello there")]
    assert tentative == ""
    assert watermark == 2.0


def test_segment_inside_trailing_margin_stays_tentative():
    segs = [Segment(4.5, 5.0, "world")]
    confirmed, tentative, watermark = confirm_segments(segs, buffer_duration_sec=5.0,
                                                         trailing_margin_sec=1.0)
    assert confirmed == []
    assert tentative == "world"
    assert watermark == 0.0


def test_mixed_confirmed_and_tentative_segments():
    segs = [Segment(0.0, 2.0, "hello"), Segment(2.0, 4.8, "how are you")]
    confirmed, tentative, watermark = confirm_segments(segs, buffer_duration_sec=5.0,
                                                         trailing_margin_sec=1.0)
    assert confirmed == [Segment(0.0, 2.0, "hello")]
    assert tentative == "how are you"
    assert watermark == 2.0


def test_empty_segments_yields_no_confirmation():
    confirmed, tentative, watermark = confirm_segments([], buffer_duration_sec=3.0,
                                                         trailing_margin_sec=1.0)
    assert confirmed == [] and tentative == "" and watermark == 0.0


def test_boundary_exactly_at_margin_is_confirmable():
    # end (4.0) == buffer_duration (5.0) - margin (1.0) -> confirmable (<=, not <).
    segs = [Segment(2.0, 4.0, "edge case")]
    confirmed, _, watermark = confirm_segments(segs, buffer_duration_sec=5.0,
                                                trailing_margin_sec=1.0)
    assert confirmed == [Segment(2.0, 4.0, "edge case")]
    assert watermark == 4.0


def test_multiple_segments_confirm_in_one_pass_after_a_pause():
    segs = [Segment(0.0, 1.0, "one"), Segment(1.0, 2.0, "two"), Segment(2.0, 3.0, "three")]
    confirmed, tentative, watermark = confirm_segments(segs, buffer_duration_sec=4.5,
                                                         trailing_margin_sec=1.0)
    assert confirmed == segs
    assert tentative == ""
    assert watermark == 3.0


def test_trim_buffer_samples_rounds_down_to_int_sample_index():
    assert trim_buffer_samples(1.5, 16000) == 24000
    assert trim_buffer_samples(0.0, 16000) == 0
