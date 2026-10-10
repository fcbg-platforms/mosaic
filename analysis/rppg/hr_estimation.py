"""
Pure signal-processing functions for turning a combined rPPG pulse signal
(from :mod:`rppg.algorithms`) into a heart-rate estimate with a confidence
metric. No mediapipe/cv2 import — only numpy/scipy, so these are directly
unit-testable against synthetic signals with a known injected frequency,
matching this project's established discipline for math this codebase's
own correctness genuinely depends on.
"""

from __future__ import annotations

import math

import numpy as np
from scipy.signal import butter, filtfilt, welch

#: Spectrum zero-padding factor (finer frequency steps than the window gives).
ZERO_PAD = 8
#: Half the chosen frequency is the heart rate when it holds a peak with at
#: least this share of the chosen peak's power (see _fundamental()).
SUBHARMONIC_RATIO = 0.3
#: ...and at least this multiple of the band's median power, so that in a
#: noisy window a noise bump at half the rate does not halve a fast heart.
SUBHARMONIC_FLOOR = 5.0
#: How far from exactly half the subharmonic peak may lie, Hz.
HARMONIC_TOL_HZ = 0.1


def bandpass_filter(
    signal: np.ndarray, fs: float, low_hz: float = 0.7, high_hz: float = 3.0, order: int = 4
) -> np.ndarray:
    """Zero-phase Butterworth bandpass filter.

    Restricts ``signal`` to the physiological pulse band. This also
    removes DC/slow drift, so no separate detrending stage is needed for
    windows this short: the bandpass's own low cutoff subsumes it.

    Parameters
    ----------
    signal : numpy.ndarray
        1-D pulse signal, uniformly sampled at ``fs`` Hz.
    fs : float
        Sample rate in Hz.
    low_hz, high_hz : float
        Passband edges in Hz. Defaults (0.7-3.0 Hz = 42-180 BPM) cover
        adult resting-to-moderate-exertion heart rate.
    order : int, default 4
        Butterworth filter order.

    Returns
    -------
    numpy.ndarray
        The filtered signal, same shape as input. If the signal is too
        short for ``scipy.signal.filtfilt``'s required padding length,
        degrades gracefully to a mean-centered (unfiltered) copy rather
        than raising. This matches this codebase's "skip/degrade, don't
        crash" discipline for too-little-data cases.
    """
    nyq = fs / 2.0
    low = low_hz / nyq
    high = min(high_hz / nyq, 0.999)  # scipy rejects a normalized cutoff of exactly 1.0
    if low <= 0 or low >= high:
        raise ValueError(f"invalid band [{low_hz}, {high_hz}] Hz for fs={fs} Hz")

    b, a = butter(order, [low, high], btype="band")
    padlen = 3 * (max(len(a), len(b)) - 1)
    if len(signal) <= padlen:
        return signal - np.mean(signal)
    return filtfilt(b, a, signal)


def estimate_hr_welch(
    signal: np.ndarray, fs: float, low_hz: float = 0.7, high_hz: float = 3.0
) -> tuple[float | None, float | None]:
    """Estimate heart rate via Welch's periodogram peak frequency.

    Parameters
    ----------
    signal : numpy.ndarray
        1-D, already-bandpass-filtered pulse signal (see
        :func:`bandpass_filter`), uniformly sampled at ``fs`` Hz.
    fs : float
        Sample rate in Hz.
    low_hz, high_hz : float
        Physiological band to search for the dominant peak.

    Returns
    -------
    bpm : float or None
        Estimated heart rate, or ``None`` if the signal is too short or
        no dominant frequency exists in the physiological band.
    snr_db : float or None
        A pulse-signal-quality metric: the ratio (in dB) of spectral
        power near the detected peak frequency and its first harmonic
        versus the remaining power in the analyzed band. Higher is more
        confident. This is a standard, documented SNR definition in the
        spirit of the rPPG literature's "pulse SNR" concept (signal power
        concentrated at the pulse frequency + its harmonic vs. spread
        elsewhere); it is **not** a verified reproduction of one
        specific paper's exact formula (unlike the CHROM/POS projections
        in :mod:`rppg.algorithms`, which were checked against primary
        sources), since no single canonical SNR formula was independently
        confirmed during this feature's research pass.

    Notes
    -----
    A signal shorter than one cycle of ``low_hz`` (about 1.4 s for the
    default band) cannot resolve any frequency in the band, so both return
    values are ``None``.
    """
    n = len(signal)
    if n < 4 or n < fs / low_hz:
        return None, None

    # Use the whole available signal as one Welch segment (equivalent to a
    # single windowed periodogram) rather than splitting into multiple
    # shorter, averaged segments — frequency resolution is fs/nperseg, and
    # rPPG analysis windows are already short (typically 5-15s), so
    # artificially shortening nperseg below the full window would coarsen
    # resolution below what the window length actually supports for no
    # real noise-averaging benefit at this scale. Capped only to bound
    # compute cost on a pathologically long input, well above any real
    # window size this is ever called with.
    nperseg = min(n, int(fs * 60))
    # Zero-padding: a 10 s window has bins 0.1 Hz (6 bpm) apart, so a heart
    # at 75 bpm could only read 72 or 78. Padding interpolates the spectrum;
    # the parabola below then places the peak between bins.
    nfft = max(nperseg, 1 << int(math.ceil(math.log2(nperseg * ZERO_PAD))))
    freqs, psd = welch(signal, fs=fs, nperseg=nperseg, nfft=nfft)

    band_mask = (freqs >= low_hz) & (freqs <= high_hz)
    if not np.any(band_mask):
        return None, None

    band_idx = np.flatnonzero(band_mask)
    k = int(band_idx[np.argmax(psd[band_idx])])
    k = _fundamental(freqs, psd, band_mask, k)
    peak_freq = min(max(_refine_peak(freqs, psd, k), low_hz), high_hz)
    bpm = peak_freq * 60.0

    ext_high = min(high_hz * 2.0, fs / 2.0)
    ext_mask = (freqs >= low_hz) & (freqs <= ext_high)
    total_power = float(np.sum(psd[ext_mask]))
    if total_power <= 0:
        # Zero power everywhere in-band means the input was degenerate
        # (e.g. a frozen/constant signal) — argmax's peak_freq above is a
        # meaningless tie-break in that case, not a real estimate. Return
        # no bpm at all rather than a fabricated reading with only the SNR
        # suppressed, matching this feature's "never fabricate a number
        # over a real gap" discipline (see roi.py/run_rppg.py's identical
        # treatment of insufficient-face-detection windows).
        return None, None

    tol_hz = 0.1
    harmonic_mask = (
        (np.abs(freqs - peak_freq) <= tol_hz) | (np.abs(freqs - 2.0 * peak_freq) <= tol_hz)
    ) & ext_mask
    signal_power = float(np.sum(psd[harmonic_mask]))
    noise_power = max(total_power - signal_power, 1e-12)

    if signal_power <= 0:
        return bpm, -60.0  # floor value: no measurable power at the peak at all

    snr_db = 10.0 * np.log10(signal_power / noise_power)
    return bpm, snr_db


def _fundamental(freqs: np.ndarray, psd: np.ndarray, band_mask: np.ndarray, k: int) -> int:
    """The index of the heartbeat's fundamental, given the strongest peak.

    A pulse wave is not a sine: its sharp upstroke puts power at twice the
    heart rate, and in a short noisy window that second harmonic can come out
    stronger than the fundamental. When there is a clear peak at half the
    chosen frequency (at least :data:`SUBHARMONIC_RATIO` of its power, and
    well above the band's noise floor, :data:`SUBHARMONIC_FLOOR`), that is the
    heart rate. A real fast heart has only noise at half its rate, which
    rarely passes both tests.
    """
    near = np.flatnonzero(band_mask & (np.abs(freqs - freqs[k] / 2.0) <= HARMONIC_TOL_HZ))
    if near.size == 0:
        return k
    j = int(near[np.argmax(psd[near])])
    is_peak = 0 < j < len(psd) - 1 and psd[j] >= psd[j - 1] and psd[j] >= psd[j + 1]
    strong = psd[j] >= SUBHARMONIC_RATIO * psd[k]
    above_noise = psd[j] >= SUBHARMONIC_FLOOR * float(np.median(psd[band_mask]))
    return j if is_peak and strong and above_noise else k


def _refine_peak(freqs: np.ndarray, psd: np.ndarray, k: int) -> float:
    """Peak frequency between bins: a parabola through the log power of the
    peak bin and its neighbours (exact for a Gaussian-shaped peak)."""
    if not 0 < k < len(psd) - 1:
        return float(freqs[k])
    a, b, c = (math.log(max(float(v), 1e-300)) for v in psd[k - 1 : k + 2])
    den = a - 2.0 * b + c
    shift = 0.5 * (a - c) / den if den < 0 else 0.0
    return float(freqs[k] + max(-0.5, min(0.5, shift)) * (freqs[1] - freqs[0]))


def median_smooth(values: np.ndarray, window: int = 3) -> np.ndarray:
    """Centered, NaN-aware median filter over per-window BPM estimates.

    Parameters
    ----------
    values : numpy.ndarray
        1-D array of per-hop BPM estimates; ``NaN`` marks a window with
        no reliable estimate (never fabricated; see
        :func:`estimate_hr_welch`).
    window : int, default 3
        Filter width in windows. ``1`` disables smoothing (returned
        unchanged). Even values are silently rounded up to the next odd
        number, matching this project's established defensive convention
        for smoothing-window parity (item 16, pose kinematics).

    Returns
    -------
    numpy.ndarray
        Same length as ``values``. A position's output is the median of
        the *valid* (non-NaN) values within its centered window, so one bad
        neighboring window doesn't blank out an otherwise-good estimate,
        and a position with no valid values in range stays ``NaN``.
    """
    values = np.asarray(values, dtype=float)
    if window <= 1:
        return values.copy()
    if window % 2 == 0:
        window += 1

    n = len(values)
    half = window // 2
    out = np.full(n, np.nan)
    for i in range(n):
        lo, hi = max(0, i - half), min(n, i + half + 1)
        chunk = values[lo:hi]
        valid = chunk[~np.isnan(chunk)]
        if valid.size > 0:
            out[i] = float(np.median(valid))
    return out
