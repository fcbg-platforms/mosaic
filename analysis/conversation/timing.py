"""
Placing audio samples on the video clock.

Video frames carry ``elapsed_ns`` timestamps (``video/timestamps_camN.csv``).
Audio is one continuous WAV. To line the two up, MOSAIC (from this release)
writes ``audio/<name>.timing.csv`` while recording: after each buffer of
samples arrives, the total sample count so far and the ``elapsed_ns`` at
arrival. Arrival is always a little late (buffering, scheduling), never
early, so the clock line is fitted under the points (:func:`fit_timing`).
That also measures the sound card's clock rate against the computer's, which
drift apart by tens of parts per million (about 0.1 s per half hour).

Older recordings have no timing file. Their audio started about when the
session did (``session_start_elapsed_ns``), within a few hundred
milliseconds; :func:`estimate_av_lag` refines that from the face on camera
(the mouth moves with the sound of its own speech).
"""

from __future__ import annotations

import csv
from dataclasses import dataclass, replace
from pathlib import Path

import numpy as np

#: Residual percentile the clock line is placed at (the earliest arrivals).
FLOOR_PERCENTILE = 2.0
#: Window over which that floor is taken, and the spread of window floors
#: beyond which the clocks are taken to have stepped apart (lost samples).
WINDOW_S = 10.0
MAX_STEP_MS = 20.0
#: Search range and minimum correlation for the audio-video lag.
MAX_LAG_S = 0.5
MIN_LAG_CORRELATION = 0.15
#: ... and how far the peak must stand above the typical correlation.
MIN_LAG_PROMINENCE = 0.1


@dataclass
class AudioClock:
    """``elapsed_ns`` of sample ``k``: ``t0_ns + k * ns_per_sample``, plus a
    piecewise-linear correction when the timing file shows the clocks
    stepping apart (samples lost in a dropout).

    ``knots_k``/``knots_ns`` (optional) give that correction at sample
    counts, interpolated between them and held beyond the ends.
    """

    t0_ns: float
    ns_per_sample: float
    method: str  # "timing_file", "session_start" or "session_start+av_lag"
    knots_k: np.ndarray | None = None
    knots_ns: np.ndarray | None = None
    #: Spread of the arrival floor along the recording, ms (0 without a file).
    step_ms: float = 0.0

    def sample_time_ns(self, k) -> np.ndarray:
        k = np.asarray(k, dtype=np.float64)
        t = self.t0_ns + k * self.ns_per_sample
        if self.knots_k is not None:
            t = t + np.interp(k, self.knots_k, self.knots_ns)
        return t

    def shifted(self, seconds: float, method: str) -> AudioClock:
        return replace(self, t0_ns=self.t0_ns + seconds * 1e9, method=method)


def read_timing_csv(path: str | Path) -> tuple[np.ndarray, np.ndarray]:
    """``(sample_count, elapsed_ns)`` rows of a timing file."""
    samples, times = [], []
    with Path(path).open(newline="") as f:
        for row in csv.DictReader(f):
            try:
                samples.append(int(row["sample_count"]))
                times.append(int(row["elapsed_ns"]))
            except (KeyError, ValueError):
                continue
    return np.asarray(samples, dtype=np.float64), np.asarray(times, dtype=np.float64)


def fit_timing(samples, elapsed_ns, rate: int) -> AudioClock | None:
    """The clock line under the arrival times: ``elapsed = t0 + k * slope``.

    The slope starts as the median over row pairs about 10 s apart and is
    refined through the window floors (below) with a Theil-Sen fit, so a
    rare step does not tilt it. The line is lowered to the earliest arrivals, since a
    sample cannot arrive before it was recorded: the
    :data:`FLOOR_PERCENTILE` of the residuals, per :data:`WINDOW_S` window.
    If those window floors differ by more than :data:`MAX_STEP_MS` (samples
    were lost when the program stalled for longer than the sound card's
    buffer), the floors become a piecewise correction instead of one
    constant, and ``step_ms`` says how large the steps were. ``None`` with
    too few rows or an implausible slope (more than 1% off the nominal
    rate).
    """
    k = np.asarray(samples, dtype=np.float64)
    t = np.asarray(elapsed_ns, dtype=np.float64)
    if k.size < 2 or np.ptp(k) <= 0:
        return None
    nominal = 1e9 / rate
    span = max(1, int(np.searchsorted(k, k[0] + WINDOW_S * rate)))
    span = min(span, k.size - 1)
    dk = k[span:] - k[:-span]
    ok = dk > 0
    if not ok.any():
        return None
    slope = float(np.median((t[span:] - t[:-span])[ok] / dk[ok]))
    if abs(slope / nominal - 1.0) > 0.01:
        return None
    windows = np.floor((k - k[0]) / (WINDOW_S * rate)).astype(np.int64)
    ids = np.unique(windows)

    def window_floors(sl):
        o = t - sl * k
        kk = np.array([np.mean(k[windows == w]) for w in ids])
        ff = np.array([np.percentile(o[windows == w], FLOOR_PERCENTILE) for w in ids])
        return kk, ff

    # Refine the slope through the window floors (the arrival times with the
    # scheduling delay taken out): Theil-Sen, the median of the slopes
    # between every pair of floors, which a step cannot tilt either.
    knots, floors_a = window_floors(slope)
    if len(knots) >= 3:
        i, j = np.triu_indices(len(knots), 1)
        slope += float(np.median((floors_a[j] - floors_a[i]) / (knots[j] - knots[i])))
        knots, floors_a = window_floors(slope)
    knots_k = knots
    step_ms = float(np.ptp(floors_a)) / 1e6
    if step_ms <= MAX_STEP_MS:
        t0 = float(np.percentile(t - slope * k, FLOOR_PERCENTILE))
        return AudioClock(t0, slope, "timing_file", step_ms=step_ms)
    base = float(floors_a[0])
    return AudioClock(
        base, slope, "timing_file", np.asarray(knots_k), floors_a - base, step_ms=step_ms
    )


def estimate_av_lag(mouth_activity, audio_db, hop_s: float) -> tuple[float, float, float]:
    """How late the audio grid is relative to the video, from the mouth.

    Cross-correlates mouth movement with audio level over
    +-:data:`MAX_LAG_S`. Returns ``(lag_s, r, prominence)``: shifting the
    audio clock by ``-lag_s`` lines them up best, with correlation ``r``,
    which stands ``prominence`` above the median over all lags (a clear peak,
    not a slow trend shared by every lag). Mouth movement
    naturally leads its sound by a few tens of milliseconds, so the result is
    good to about 0.1 s.
    """
    v = np.asarray(mouth_activity, dtype=np.float64)
    a = np.asarray(audio_db, dtype=np.float64)
    n = min(len(v), len(a))
    v, a = v[:n], a[:n]
    ok = np.isfinite(v) & np.isfinite(a)
    if ok.sum() < int(10.0 / hop_s):
        return 0.0, 0.0, 0.0
    v = np.where(ok, (v - v[ok].mean()) / (v[ok].std() + 1e-12), 0.0)
    a = np.where(ok, (a - a[ok].mean()) / (a[ok].std() + 1e-12), 0.0)
    max_lag = int(round(MAX_LAG_S / hop_s))
    best, best_r = 0, -np.inf
    all_r = []
    for lag in range(-max_lag, max_lag + 1):
        # Audio frame i + lag goes with video frame i.
        if lag >= 0:
            x, y, w = v[: n - lag], a[lag:], ok[: n - lag] & ok[lag:]
        else:
            x, y, w = v[-lag:], a[: n + lag], ok[-lag:] & ok[: n + lag]
        if w.sum() < 10:
            continue
        r = float(np.mean(x[w] * y[w]))
        all_r.append(r)
        if r > best_r:
            best, best_r = lag, r
    if not all_r:
        return 0.0, 0.0, 0.0
    return best * hop_s, best_r, best_r - float(np.median(all_r))


def lag_is_reliable(r: float, prominence: float) -> bool:
    """Whether an :func:`estimate_av_lag` result should be trusted."""
    return r >= MIN_LAG_CORRELATION and prominence >= MIN_LAG_PROMINENCE
