"""
Zero-lag smoothing of gaze over time.

The One Euro filter (Casiez, Roussel and Vogel, CHI 2012) adapts its cutoff
to the signal's speed: strong smoothing while the gaze is still (fixations),
little while it moves fast (saccades), so jitter is removed without blurring
real gaze shifts. A causal filter still lags, so :func:`smooth_series` runs
it forwards and backwards and averages the two passes: their lags largely
cancel (exactly for a fixed cutoff, closely for this adaptive one).

Gaps (ticks without an estimate) split the series: each stretch is
smoothed on its own and nothing is invented across a gap.
"""

from __future__ import annotations

import math

import numpy as np


def _alpha(cutoff_hz: float, dt: float) -> float:
    tau = 1.0 / (2.0 * math.pi * cutoff_hz)
    return 1.0 / (1.0 + tau / dt)


def one_euro(
    values, times_s, min_cutoff_hz: float = 1.0, beta: float = 0.3, d_cutoff_hz: float = 1.0
) -> np.ndarray:
    """Causal One Euro filter over a multi-dimensional series.

    Parameters
    ----------
    values : array_like
        ``(T, D)`` samples.
    times_s : array_like
        ``(T,)`` strictly increasing sample times in seconds.
    min_cutoff_hz : float
        Cutoff while the signal is still. Lower means smoother fixations.
    beta : float
        How fast the cutoff rises with speed. Higher means less lag on
        fast movements.
    d_cutoff_hz : float
        Cutoff of the speed estimate itself.

    Returns
    -------
    numpy.ndarray
        ``(T, D)`` filtered samples.
    """
    x = np.asarray(values, dtype=np.float64)
    if x.ndim == 1:
        x = x[:, None]
    t = np.asarray(times_s, dtype=np.float64)
    out = np.empty_like(x)
    if len(x) == 0:
        return out
    out[0] = x[0]
    dx_prev = np.zeros(x.shape[1])
    for i in range(1, len(x)):
        dt = max(t[i] - t[i - 1], 1e-6)
        dx = (x[i] - out[i - 1]) / dt
        a_d = _alpha(d_cutoff_hz, dt)
        dx_hat = a_d * dx + (1.0 - a_d) * dx_prev
        cutoff = min_cutoff_hz + beta * float(np.linalg.norm(dx_hat))
        a = _alpha(cutoff, dt)
        out[i] = a * x[i] + (1.0 - a) * out[i - 1]
        dx_prev = dx_hat
    return out


def smooth_series(values, times_s, valid, max_gap_s: float = 0.5, **one_euro_kwargs) -> np.ndarray:
    """Forward-backward One Euro over the valid stretches of a series.

    Parameters
    ----------
    values : array_like
        ``(T, D)`` samples (invalid rows may hold anything).
    times_s : array_like
        ``(T,)`` sample times in seconds.
    valid : array_like
        ``(T,)`` bool, which rows hold real estimates.
    max_gap_s : float
        A gap longer than this starts a new stretch.
    **one_euro_kwargs
        Passed to :func:`one_euro`.

    Returns
    -------
    numpy.ndarray
        ``(T, D)``; invalid rows are returned unchanged.
    """
    x = np.asarray(values, dtype=np.float64)
    squeeze = x.ndim == 1
    if squeeze:
        x = x[:, None]
    t = np.asarray(times_s, dtype=np.float64)
    ok = np.asarray(valid, dtype=bool)
    out = x.copy()
    idx = np.flatnonzero(ok)
    if idx.size == 0:
        return out[:, 0] if squeeze else out

    # Split the valid samples wherever time jumps by more than max_gap_s.
    breaks = np.flatnonzero(np.diff(t[idx]) > max_gap_s) + 1
    for run in np.split(idx, breaks):
        if run.size < 3:
            continue
        fwd = one_euro(x[run], t[run], **one_euro_kwargs)
        bwd = one_euro(x[run][::-1], -t[run][::-1], **one_euro_kwargs)[::-1]
        out[run] = (fwd + bwd) / 2.0
    return out[:, 0] if squeeze else out


def smooth_directions(directions, times_s, valid, **kwargs) -> np.ndarray:
    """:func:`smooth_series` for unit vectors: smooths the components and
    renormalises, which is accurate for the small angular steps between
    neighbouring ticks."""
    d = smooth_series(directions, times_s, valid, **kwargs)
    n = np.linalg.norm(d, axis=1, keepdims=True)
    return np.where(n > 1e-12, d / np.maximum(n, 1e-12), d)
