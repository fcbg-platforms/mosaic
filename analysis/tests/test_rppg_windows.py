"""run_rppg.window_spans: analysis windows stay inside the recording."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from run_rppg import window_spans


def test_windows_stay_inside_and_the_last_ends_on_the_last_frame():
    spans = window_spans(1000.0, 24500.0, 10000.0, 2000.0)
    assert all(1000.0 <= a < b <= 24500.0 for a, b in spans)
    assert all(b - a == pytest.approx(10000.0) for a, b in spans)
    assert spans[0][0] == 1000.0
    assert spans[-1] == (14500.0, 24500.0)  # the extra one, ending on the last frame
    starts = [a for a, _ in spans]
    assert starts[:7] == [1000.0 + 2000.0 * k for k in range(7)]  # 1..13 s


def test_a_short_recording_is_one_window():
    assert window_spans(0.0, 7000.0, 10000.0, 2000.0) == [(0.0, 7000.0)]


def test_no_extra_window_when_the_last_nearly_ends_on_the_last_frame():
    spans = window_spans(0.0, 20500.0, 10000.0, 2000.0)
    assert spans[-1] == (10000.0, 20000.0)  # 500 ms short: no extra window
    spans = window_spans(0.0, 21500.0, 10000.0, 2000.0)
    assert spans[-1] == (11500.0, 21500.0)
