"""Pure-numeric tests for voice/spectro.py — numpy only, no parselmouth.

These cover the decisions that are wrong-but-plausible: reducing on the wrong
axis, averaging decibels instead of power, quantising before reducing, or
letting the pitch tracker's readings from room tone through. Each of those
produces an image or a contour that looks entirely reasonable and misrepresents
the recording, which is exactly the class of bug a picture cannot reveal.
"""

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from voice.spectro import (
    MAX_COLUMNS,
    MIN_COLUMNS,
    accumulate_column_max,
    auto_pitch_range,
    column_indices,
    db_to_uint8,
    decimate_track,
    drop_short_voiced_runs,
    dynamic_range,
    fill_empty_columns,
    reduce_freq_mean,
    spectrogram_time_step,
    target_columns,
    track_step_ms,
)

# ── Column budgeting ─────────────────────────────────────────────────────────


def test_target_columns_clamps_both_ends():
    assert target_columns(0.5) == MIN_COLUMNS
    assert target_columns(10.0) == MIN_COLUMNS  # 200 cols would be too few
    assert target_columns(60_000.0) == MAX_COLUMNS


def test_target_columns_caps_so_an_hour_costs_the_same_as_a_minute():
    # The whole point of the cap: draw cost must not scale with recording length.
    assert target_columns(600.0) == target_columns(3600.0) == MAX_COLUMNS


def test_spectrogram_time_step_escalates_with_duration():
    assert spectrogram_time_step(60.0) == 0.002
    assert spectrogram_time_step(600.0) == 0.002
    assert spectrogram_time_step(601.0) == 0.004
    assert spectrogram_time_step(1801.0) == 0.008


def test_track_step_keeps_an_hour_under_the_frame_budget():
    step = track_step_ms(3600.0)
    assert step % 10.0 == 0  # whole multiple of the analysis step
    assert 3600.0 * 1000.0 / step <= 60_000


def test_track_step_leaves_short_recordings_at_full_resolution():
    assert track_step_ms(60.0) == 10.0


# ── Reduction ────────────────────────────────────────────────────────────────


def test_column_indices_span_the_whole_grid_without_escaping_it():
    times = np.linspace(0.0, 10.0, 1000)
    cols = column_indices(times, 20, 0.0, 10.0)
    assert cols.min() == 0
    # The final frame sits exactly on the upper bound and must land in the last
    # column, not one past it — the tail of the recording is otherwise silently
    # missing from the image.
    assert cols.max() == 19
    assert np.all(np.diff(cols) >= 0)


def test_accumulate_column_max_keeps_the_loudest_event_in_each_column():
    # A single loud frame buried in quiet: a mean would erase it, which is how
    # plosives disappear at long durations.
    db = np.full((4, 100), -60.0)
    db[:, 37] = 0.0
    out = np.zeros((4, 2))
    filled = np.zeros(2, dtype=bool)
    accumulate_column_max(out, filled, db, np.array([0] * 50 + [1] * 50))
    assert out[0, 0] == pytest.approx(0.0)
    assert out[0, 1] == pytest.approx(-60.0)
    assert filled.all()


def test_accumulate_column_max_combines_across_successive_chunks():
    # THE streaming property: chunk 2's louder frame must beat chunk 1's in the
    # column they share. Getting this wrong is invisible — the image is simply
    # quieter than the recording.
    out = np.zeros((1, 2))
    filled = np.zeros(2, dtype=bool)
    accumulate_column_max(out, filled, np.array([[-30.0]]), np.array([0]))
    accumulate_column_max(out, filled, np.array([[-10.0]]), np.array([0]))
    assert out[0, 0] == pytest.approx(-10.0)


def test_fill_empty_columns_carries_forward_and_back():
    # A hole renders as a black stripe, which reads as silence rather than as
    # "no sample landed here".
    out = np.array([[0.0, 5.0, 0.0, 7.0, 0.0]])
    filled = np.array([False, True, False, True, False])
    fill_empty_columns(out, filled)
    assert out[0, 0] == pytest.approx(5.0)  # leading gap back-filled
    assert out[0, 2] == pytest.approx(5.0)  # interior gap carried forward
    assert out[0, 4] == pytest.approx(7.0)  # trailing gap carried forward


def test_fill_empty_columns_on_an_entirely_empty_grid_is_a_no_op():
    out = np.zeros((1, 3))
    fill_empty_columns(out, np.zeros(3, dtype=bool))
    assert np.all(out == 0.0)


# THE averaging-in-the-wrong-domain test. 0 dB and 20 dB are powers 1 and 100;
# their mean is 50.5, i.e. ~17.0 dB. Averaging the decibels instead gives 10.0
# and quietly darkens every band in the image.
def test_reduce_freq_mean_averages_power_not_decibels():
    power = np.array([[1.0], [100.0]])  # 0 dB and 20 dB
    out = reduce_freq_mean(power, 1)
    assert out.shape == (1, 1)
    assert 10.0 * np.log10(out[0, 0]) == pytest.approx(17.0, abs=0.1)


def test_reduce_freq_mean_leaves_fewer_rows_than_requested_alone():
    power = np.ones((10, 5))
    assert reduce_freq_mean(power, 256).shape == (10, 5)


# ── dB scaling ───────────────────────────────────────────────────────────────


def test_dynamic_range_ignores_a_single_outlier_peak():
    # One clipped sample must not compress the whole recording into the floor.
    db = np.concatenate([np.full(10_000, -40.0), np.array([120.0])])
    floor, ceil = dynamic_range(db, top_db=70.0)
    assert ceil < 100.0
    assert ceil - floor == pytest.approx(70.0)


def test_db_to_uint8_maps_the_endpoints_and_clips_outside():
    db = np.array([-100.0, -20.0, 0.0, 20.0, 100.0])
    out = db_to_uint8(db, floor_db=-20.0, ceil_db=20.0)
    assert out[0] == 0  # below floor
    assert out[1] == 0
    assert out[3] == 255
    assert out[4] == 255  # above ceiling
    assert 120 <= out[2] <= 135  # midpoint


def test_db_to_uint8_sends_nan_to_the_floor():
    out = db_to_uint8(np.array([np.nan]), floor_db=-20.0, ceil_db=20.0)
    assert out[0] == 0


# ── Pitch post-filtering ─────────────────────────────────────────────────────


def test_suppress_low_intensity_pitch_drops_room_tone_readings():
    from voice.spectro import suppress_low_intensity_pitch

    pitch = np.array([120.0, 118.0, 119.0, 121.0])
    intensity = np.array([70.0, 70.0, 20.0, 71.0])  # frame 2 is near-silent
    out = suppress_low_intensity_pitch(pitch, intensity, drop_below_db=35.0)
    assert out[0] > 0 and out[1] > 0 and out[3] > 0
    assert out[2] == 0.0


def test_drop_short_voiced_runs_removes_specks_but_keeps_real_phonation():
    pitch = np.array([0, 100, 0, 0, 110, 111, 0, 120, 121, 122, 0], dtype=float)
    out = drop_short_voiced_runs(pitch, min_frames=3)
    assert out[1] == 0.0  # 1-frame speck removed
    assert out[4] == 0.0 and out[5] == 0.0  # 2-frame run removed
    assert out[7] > 0 and out[8] > 0 and out[9] > 0  # 3-frame run kept


def test_drop_short_voiced_runs_handles_a_run_touching_the_end():
    # The trailing run has no unvoiced frame after it to trigger the flush.
    out = drop_short_voiced_runs(np.array([0.0, 0.0, 150.0]), min_frames=3)
    assert out[2] == 0.0


# ── Decimation ───────────────────────────────────────────────────────────────


def test_decimate_track_voiced_aware_ignores_unvoiced_zeros():
    # A plain mean of [100, 100, 0, 0] is 50 Hz — an octave below anything real.
    out = decimate_track(np.array([100.0, 102.0, 0.0, 0.0]), factor=4, voiced_aware=True)
    assert out[0] == pytest.approx(101.0)


def test_decimate_track_voiced_aware_marks_mostly_unvoiced_blocks_unvoiced():
    out = decimate_track(np.array([100.0, 0.0, 0.0, 0.0]), factor=4, voiced_aware=True)
    assert out[0] == 0.0


def test_decimate_track_factor_one_is_identity():
    values = np.array([1.0, 2.0, 3.0])
    assert np.allclose(decimate_track(values, 1), values)


# ── Auto pitch range ─────────────────────────────────────────────────────────


def test_auto_pitch_range_narrows_to_the_voices_present():
    voiced = np.random.default_rng(0).normal(200.0, 15.0, 500)
    floor, ceiling = auto_pitch_range(voiced)
    assert floor < 200.0 < ceiling
    assert ceiling - floor < 540.0  # narrower than the 60-600 fallback


def test_auto_pitch_range_falls_back_when_there_is_too_little_voicing():
    # Percentiles of a nearly-empty array are noise, and a NaN range yields an
    # invisible pitch line with no error reported anywhere.
    floor, ceiling = auto_pitch_range(np.array([180.0, 190.0]))
    assert (floor, ceiling) == (60.0, 600.0)


def test_auto_pitch_range_never_returns_nan_or_an_inverted_range():
    for sample in (np.zeros(500), np.full(500, 1e-6), np.full(500, 5000.0)):
        floor, ceiling = auto_pitch_range(sample)
        assert not np.isnan(floor) and not np.isnan(ceiling)
        assert ceiling > floor
