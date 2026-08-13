"""
Pure-logic tests for rppg/hr_estimation.py's bandpass_filter()/
estimate_hr_welch()/median_smooth() — no mediapipe/cv2 import required.
"""
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from rppg.hr_estimation import bandpass_filter, estimate_hr_welch, median_smooth


def _sinusoid(freq_hz: float, fs: float, duration_s: float, amplitude: float = 1.0) -> np.ndarray:
    t = np.arange(int(fs * duration_s)) / fs
    return amplitude * np.sin(2 * np.pi * freq_hz * t)


class TestBandpassFilter:
    FS = 30.0
    DURATION_S = 10.0

    def test_removes_strong_out_of_band_low_frequency_component(self):
        in_band = _sinusoid(1.2, self.FS, self.DURATION_S, amplitude=1.0)      # 72 BPM, in-band
        out_of_band = _sinusoid(0.1, self.FS, self.DURATION_S, amplitude=10.0)  # far below 0.7 Hz
        signal = in_band + out_of_band
        filtered = bandpass_filter(signal, self.FS, low_hz=0.7, high_hz=3.0)

        # The huge low-frequency component should be almost entirely gone —
        # filtered signal's amplitude should look like the in-band component
        # alone, not the (much larger) combined signal.
        assert np.std(filtered) < 2.0 * np.std(in_band)

    def test_preserves_an_in_band_component_reasonably_well(self):
        in_band = _sinusoid(1.2, self.FS, self.DURATION_S, amplitude=1.0)
        filtered = bandpass_filter(in_band, self.FS, low_hz=0.7, high_hz=3.0)
        # Zero-phase filtfilt shouldn't drastically attenuate a component
        # well inside the passband.
        assert np.std(filtered) == pytest.approx(np.std(in_band), rel=0.3)

    def test_degrades_gracefully_on_too_short_signal_instead_of_raising(self):
        short_signal = np.array([1.0, 2.0, 1.5])
        result = bandpass_filter(short_signal, fs=30.0)
        assert np.all(np.isfinite(result))
        assert len(result) == len(short_signal)

    def test_raises_on_invalid_band(self):
        with pytest.raises(ValueError):
            bandpass_filter(np.ones(100), fs=2.0, low_hz=1.0, high_hz=0.9)


class TestEstimateHrWelch:
    FS = 30.0
    DURATION_S = 15.0

    def test_recovers_clean_known_bpm_with_high_snr(self):
        pulse_hz = 1.2   # 72 BPM
        signal = _sinusoid(pulse_hz, self.FS, self.DURATION_S)
        bpm, snr_db = estimate_hr_welch(signal, self.FS)
        assert bpm is not None and snr_db is not None
        assert bpm == pytest.approx(pulse_hz * 60.0, abs=2.0)
        assert snr_db > 0.0

    def test_low_confidence_on_pure_noise(self):
        rng = np.random.default_rng(7)
        noise = rng.standard_normal(int(self.FS * self.DURATION_S))
        bpm, snr_db = estimate_hr_welch(noise, self.FS)
        # Welch always finds SOME peak in the band — that's expected and
        # realistic; what must be true is that its reported confidence is
        # low, not that bpm is None.
        assert bpm is not None
        assert snr_db is not None and snr_db < 5.0

    def test_returns_none_for_too_short_signal(self):
        bpm, snr_db = estimate_hr_welch(np.array([1.0, 2.0]), fs=30.0)
        assert bpm is None and snr_db is None

    def test_bpm_within_requested_physiological_band(self):
        # Inject a pulse OUTSIDE the requested band and confirm the
        # returned bpm still falls inside [low_hz, high_hz]*60 — i.e. the
        # search is genuinely restricted to the requested band, not just
        # finding the global spectral peak.
        signal = _sinusoid(4.0, self.FS, self.DURATION_S)   # 240 BPM, outside default 42-180
        bpm, _ = estimate_hr_welch(signal, self.FS, low_hz=0.7, high_hz=3.0)
        assert bpm is not None
        assert 0.7 * 60.0 <= bpm <= 3.0 * 60.0


class TestMedianSmooth:
    def test_window_one_returns_unchanged_copy(self):
        values = np.array([70.0, 72.0, 150.0, 71.0])
        result = median_smooth(values, window=1)
        assert result == pytest.approx(values)
        assert result is not values   # must be a copy, not the same array object

    def test_smooths_a_single_outlier(self):
        values = np.array([70.0, 71.0, 150.0, 72.0, 70.0])
        result = median_smooth(values, window=3)
        # The outlier at index 2 should be pulled toward its neighbors,
        # not left untouched.
        assert result[2] < 150.0

    def test_even_window_rounds_up_to_next_odd(self):
        values = np.array([70.0, 72.0, 71.0, 73.0])
        result_even = median_smooth(values, window=2)
        result_odd = median_smooth(values, window=3)
        assert result_even == pytest.approx(result_odd)

    def test_nan_entries_excluded_not_propagated(self):
        values = np.array([70.0, np.nan, 72.0])
        result = median_smooth(values, window=3)
        # The NaN at index 1 must not blank out its valid neighbors —
        # each position's median is computed only over the valid values in
        # its window.
        assert not np.isnan(result[0])
        assert not np.isnan(result[2])

    def test_all_nan_window_stays_nan(self):
        values = np.array([np.nan, np.nan, np.nan])
        result = median_smooth(values, window=3)
        assert np.all(np.isnan(result))
