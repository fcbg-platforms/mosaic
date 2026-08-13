"""
Pure-logic tests for rppg/algorithms.py's green_signal()/chrom_signal()/
pos_signal()/normalize_channels() — no mediapipe/cv2/scipy import required.

Scope note, stated honestly rather than overclaimed: these tests verify
(1) that each backend's combination math correctly recovers a known
injected pulse frequency from a clean synthetic signal, (2) exact,
provable mathematical properties of normalize_channels() (invariance to a
per-channel multiplicative scale), and (3) that CHROM/POS suppress a
shared "white" (equal-across-channels) interferer more than the naive
Green backend does, for one concrete representative skin-tone baseline.
Point (3) is deliberately framed as "verified for a representative
example," not "proven in general" — a fully general proof would require
reproducing the CHROM/POS papers' own skin-tone-standardization
derivation steps, which weren't independently verified during this
feature's research pass (see algorithms.py's own docstrings for exactly
what was and wasn't confirmed against a primary source).
"""
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from rppg.algorithms import BACKENDS, chrom_signal, green_signal, normalize_channels, pos_signal


def _dominant_freq_hz(signal: np.ndarray, fs: float) -> float:
    """Test-local helper: FFT-based dominant frequency, restricted to a
    plausible pulse band, so a DC/near-zero bin never wins by default."""
    n = len(signal)
    freqs = np.fft.rfftfreq(n, d=1.0 / fs)
    mags = np.abs(np.fft.rfft(signal - signal.mean()))
    band = (freqs >= 0.5) & (freqs <= 4.0)
    return float(freqs[band][np.argmax(mags[band])])


def _synthetic_clean_rgb(pulse_hz: float, fs: float, duration_s: float,
                          baseline=(180.0, 120.0, 90.0)) -> np.ndarray:
    """A clean (no interferer) synthetic RGB signal: a fixed skin-tone
    baseline plus a small pulse-frequency oscillation, weighted per
    channel by typical relative pulsatile amplitude (green strongest, red
    medium, blue weakest — directionally well-established in the rPPG
    literature; the exact ratios below are a simplified, illustrative
    model, not a validated skin-optics simulation)."""
    t = np.arange(int(fs * duration_s)) / fs
    pulse = np.sin(2 * np.pi * pulse_hz * t)
    r0, g0, b0 = baseline
    r = r0 + 0.8 * pulse
    g = g0 + 1.0 * pulse
    b = b0 + 0.3 * pulse
    return np.column_stack([r, g, b])


def _synthetic_rgb_with_white_interferer(pulse_hz: float, interferer_hz: float, fs: float,
                                          duration_s: float,
                                          baseline=(180.0, 120.0, 90.0)) -> np.ndarray:
    """As above, plus a large "white"/specular-like interferer — an
    identical additive term added to all three channels equally, at a
    different, out-of-band-adjacent frequency — the standard synthetic
    model for the kind of shared illumination/specular artifact CHROM/POS
    are designed to suppress relative to a naive single-channel method."""
    rgb = _synthetic_clean_rgb(pulse_hz, fs, duration_s, baseline)
    t = np.arange(int(fs * duration_s)) / fs
    interferer = 15.0 * np.sin(2 * np.pi * interferer_hz * t)   # much larger than the pulse
    return rgb + interferer[:, None]


class TestFrequencyRecovery:
    """Each backend, on a clean synthetic signal, must recover the
    injected pulse frequency — the core, directly-verifiable correctness
    property of the combination math itself."""

    FS = 30.0
    DURATION_S = 10.0
    PULSE_HZ = 1.2   # 72 BPM

    @pytest.mark.parametrize("backend_name", ["green", "chrom", "pos"])
    def test_recovers_known_pulse_frequency(self, backend_name):
        rgb = _synthetic_clean_rgb(self.PULSE_HZ, self.FS, self.DURATION_S)
        combined = BACKENDS[backend_name](rgb)
        recovered = _dominant_freq_hz(combined, self.FS)
        assert recovered == pytest.approx(self.PULSE_HZ, abs=0.15)


class TestNormalizeChannels:
    def test_divides_each_channel_by_its_own_temporal_mean(self):
        rgb = np.array([[100.0, 50.0, 20.0], [200.0, 100.0, 40.0], [300.0, 150.0, 60.0]])
        result = normalize_channels(rgb)
        assert result[:, 0] == pytest.approx(rgb[:, 0] / 200.0)
        assert result[:, 1] == pytest.approx(rgb[:, 1] / 100.0)
        assert result[:, 2] == pytest.approx(rgb[:, 2] / 40.0)

    def test_invariant_to_a_per_channel_multiplicative_scale(self):
        # Cn = C/mean(C) is exactly unchanged if a channel is uniformly
        # scaled (e.g. a constant brightness/gain difference between two
        # recordings) — a real, provable property, not an approximation.
        rng = np.random.default_rng(42)
        rgb = 100.0 + 20.0 * rng.standard_normal((50, 3))
        scaled = rgb * np.array([1.4, 0.7, 2.1])
        assert normalize_channels(scaled) == pytest.approx(normalize_channels(rgb))

    def test_raises_on_degenerate_zero_mean_channel(self):
        rgb = np.zeros((10, 3))
        with pytest.raises(ValueError):
            normalize_channels(rgb)


class TestDegenerateWindows:
    """A window with essentially no variation (e.g. a flat, static ROI)
    must not crash or produce NaN/inf — matches this codebase's
    established "skip/degrade, don't crash" discipline."""

    @pytest.mark.parametrize("fn", [chrom_signal, pos_signal])
    def test_flat_window_returns_zeros_not_nan_or_crash(self, fn):
        rgb = np.tile([180.0, 120.0, 90.0], (20, 1))   # perfectly constant — zero variance
        result = fn(rgb)
        assert np.all(np.isfinite(result))
        assert np.allclose(result, 0.0)


class TestWhiteInterfererSuppression:
    """Representative-example check (see module docstring for the honest
    scope note): for one concrete skin-tone baseline, CHROM/POS suppress
    a large shared "white" interferer more than the naive Green backend,
    which has zero suppression of any such term by construction (Green is
    just the raw green channel — the interferer passes straight through
    with no cancellation whatsoever)."""

    FS = 30.0
    DURATION_S = 10.0
    PULSE_HZ = 1.2
    INTERFERER_HZ = 0.3   # a plausible ambient-flicker-style frequency, clearly separate from pulse

    def test_chrom_and_pos_suppress_shared_interferer_more_than_green(self):
        rgb = _synthetic_rgb_with_white_interferer(self.PULSE_HZ, self.INTERFERER_HZ,
                                                     self.FS, self.DURATION_S)

        def interferer_to_pulse_power_ratio(signal: np.ndarray) -> float:
            n = len(signal)
            freqs = np.fft.rfftfreq(n, d=1.0 / self.FS)
            mags = np.abs(np.fft.rfft(signal - signal.mean()))
            interferer_power = mags[np.argmin(np.abs(freqs - self.INTERFERER_HZ))]
            pulse_power = mags[np.argmin(np.abs(freqs - self.PULSE_HZ))]
            return interferer_power / max(pulse_power, 1e-9)

        green_ratio = interferer_to_pulse_power_ratio(green_signal(rgb))
        chrom_ratio = interferer_to_pulse_power_ratio(chrom_signal(rgb))
        pos_ratio = interferer_to_pulse_power_ratio(pos_signal(rgb))

        assert chrom_ratio < green_ratio
        assert pos_ratio < green_ratio
