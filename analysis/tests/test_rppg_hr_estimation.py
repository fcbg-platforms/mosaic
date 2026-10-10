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
        in_band = _sinusoid(1.2, self.FS, self.DURATION_S, amplitude=1.0)  # 72 BPM, in-band
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
        pulse_hz = 1.2  # 72 BPM
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
        signal = _sinusoid(4.0, self.FS, self.DURATION_S)  # 240 BPM, outside default 42-180
        bpm, _ = estimate_hr_welch(signal, self.FS, low_hz=0.7, high_hz=3.0)
        assert bpm is not None
        assert 0.7 * 60.0 <= bpm <= 3.0 * 60.0


class TestMedianSmooth:
    def test_window_one_returns_unchanged_copy(self):
        values = np.array([70.0, 72.0, 150.0, 71.0])
        result = median_smooth(values, window=1)
        assert result == pytest.approx(values)
        assert result is not values  # must be a copy, not the same array object

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


class TestWindowAccuracy:
    """The two errors the golden session exposed in 10 s windows."""

    FS = 25.0

    @staticmethod
    def _pulse(bpm, seconds, fs, harmonic=0.0, noise=0.0, seed=0):
        rng = np.random.default_rng(seed)
        t = np.arange(int(seconds * fs)) / fs
        f = bpm / 60.0
        return (
            np.sin(2 * np.pi * f * t)
            + harmonic * np.sin(2 * np.pi * 2 * f * t + 0.7)
            + rng.normal(0, noise, t.size)
        )

    def test_rates_between_frequency_bins_are_read_to_within_a_bpm(self):
        # 10 s windows have 6 bpm bins: 75 used to read as 72 or 78.
        for bpm in (63.0, 75.0, 87.5, 101.0, 118.0):
            est, _ = estimate_hr_welch(self._pulse(bpm, 10.0, self.FS), self.FS)
            assert est == pytest.approx(bpm, abs=1.0), bpm

    def test_a_strong_second_harmonic_is_not_taken_for_the_heart_rate(self):
        # A sharp pulse wave can put more power at twice the rate.
        sig = self._pulse(75.0, 10.0, self.FS, harmonic=1.3, noise=0.2, seed=1)
        est, _ = estimate_hr_welch(sig, self.FS)
        assert est == pytest.approx(75.0, abs=1.5)

    def test_a_genuinely_fast_heart_stays_fast(self):
        sig = self._pulse(150.0, 10.0, self.FS, harmonic=0.3, noise=0.2, seed=2)
        est, _ = estimate_hr_welch(sig, self.FS)
        assert est == pytest.approx(150.0, abs=1.5)

    def test_noise_rarely_halves_a_fast_heart(self):
        # The halving rule must not mistake a noise bump at half the rate for
        # the fundamental; 30 fps, heavy noise, many windows.
        fs, ok = 30.0, 0
        for seed in range(100):
            sig = bandpass_filter(self._pulse(150.0, 10.0, fs, 0.3, 2.0, seed), fs)
            est, _ = estimate_hr_welch(sig, fs)
            ok += abs(est - 150.0) < 6.0
        assert ok >= 85

    def test_noisy_rates_between_bins_are_read_closely(self):
        fs = 30.0
        for seed, bpm in enumerate((58.0, 75.0, 93.0)):
            sig = bandpass_filter(self._pulse(bpm, 10.0, fs, 0.3, 0.5, seed), fs)
            est, _ = estimate_hr_welch(sig, fs)
            assert est == pytest.approx(bpm, abs=2.0), bpm

    def test_shorter_than_one_cycle_of_the_band_gives_nothing(self):
        for n in (4, 8, 20, 40):
            assert estimate_hr_welch(np.sin(np.arange(n)), 30.0) == (None, None)

    def test_the_rate_stays_inside_the_band(self):
        rng = np.random.default_rng(5)
        for _ in range(50):
            est, _ = estimate_hr_welch(rng.normal(size=300), 30.0, 0.7, 3.0)
            assert 42.0 <= est <= 180.0
