"""Windowed cross-correlation (WCC) between two people's signals, tested
against surrogate pairs.

WCC (Boker, Rotondo, Xu and King, 2002) follows a coupling that changes over
time: within each short window, person B's signal is correlated with person A's
at a range of lags, and the peak correlation and its lag are kept. A positive
lag means B follows A by that much (A leads).

Two signals that are not coupled at all still give peak correlations well above
zero, because the peak is the best of many lags. So the real pair is compared
with surrogate pairs: B's signal circularly shifted by a random offset, which
keeps each person's own rhythm but breaks any timing between them (Moulder,
Boker, Ramseyer and Tschacher, 2018). Synchrony is the real mean peak
correlation minus the surrogates' mean; the p-value is the share of surrogates
at least as high.
"""

from __future__ import annotations

import numpy as np

N_SURROGATES = 100
#: A window counts only when this share of its samples is present in both.
MIN_VALID = 0.7


def _windows(x: np.ndarray, starts: np.ndarray, w: int) -> np.ndarray:
    return x[starts[:, None] + np.arange(w)[None, :]]


def _corr_rows(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    """Pearson r per row, over the samples present in both; NaN when too few
    are present or either side is flat."""
    ok = np.isfinite(a) & np.isfinite(b)
    n = ok.sum(axis=1)
    a = np.where(ok, a, 0.0)
    b = np.where(ok, b, 0.0)
    with np.errstate(invalid="ignore", divide="ignore"):
        ma = a.sum(axis=1) / n
        mb = b.sum(axis=1) / n
        da = np.where(ok, a - ma[:, None], 0.0)
        db = np.where(ok, b - mb[:, None], 0.0)
        r = (da * db).sum(axis=1) / np.sqrt((da * da).sum(axis=1) * (db * db).sum(axis=1))
    r[n < MIN_VALID * a.shape[1]] = np.nan
    return r


def wcc(x, y, fs: float, window_s: float, step_s: float, max_lag_s: float) -> dict:
    """Peak correlation and its lag per window.

    Parameters
    ----------
    x, y : array_like
        The two people's signals on one time grid (``fs`` samples per second),
        NaN where not measured.
    fs : float
        Samples per second.
    window_s, step_s, max_lag_s : float
        Window length, the step between windows, and the largest lag tried
        either way.

    Returns
    -------
    dict
        ``t_s`` (window centres, seconds from the grid's start), ``peak_r``,
        ``peak_lag_s`` (positive: B follows A) and ``lag0_r``, one per window;
        NaN where a window had too little data.
    """
    x = np.asarray(x, float)
    y = np.asarray(y, float)
    w = max(2, int(round(window_s * fs)))
    step = max(1, int(round(step_s * fs)))
    lag = int(round(max_lag_s * fs))
    n = min(len(x), len(y))
    starts = np.arange(lag, n - w - lag + 1, step)
    empty = np.array([])
    if starts.size == 0:
        return {"t_s": empty, "peak_r": empty, "peak_lag_s": empty, "lag0_r": empty}
    xa = _windows(x, starts, w)
    rs = np.full((starts.size, 2 * lag + 1), np.nan)
    for k, L in enumerate(range(-lag, lag + 1)):
        rs[:, k] = _corr_rows(xa, _windows(y, starts + L, w))
    has = np.isfinite(rs).any(axis=1)
    best = np.zeros(starts.size, int)
    best[has] = np.nanargmax(rs[has], axis=1)
    peak = np.where(has, rs[np.arange(starts.size), best], np.nan)
    lags = np.where(has, (best - lag) / fs, np.nan)
    return {
        "t_s": (starts + w / 2.0) / fs,
        "peak_r": peak,
        "peak_lag_s": lags,
        "lag0_r": rs[:, lag],
    }


def synchrony(
    x, y, fs: float, window_s: float, step_s: float, max_lag_s: float, seed: int = 0
) -> dict | None:
    """WCC of the real pair, summarised and compared with surrogates.

    Returns ``None`` when no window had enough data. Otherwise ``series``
    (per window: ``t_s``, ``peak_r``, ``peak_lag_s``) and ``summary``:

    - ``mean_peak_r``: the real pair's mean peak correlation (over windows,
      through Fisher's z);
    - ``surrogate_mean_peak_r`` and ``surrogate_p95``: the same for the
      surrogate pairs;
    - ``synchrony``: the real minus the surrogates' mean;
    - ``p_value``: the share of surrogates at least as high as the real pair;
    - ``a_leads_pct`` / ``b_leads_pct``: of the windows whose peak beats the
      surrogates' 95th percentile of single-window peaks, the share where B
      follows A (positive lag) and where A follows B;
    - ``median_lag_s``: the median lag of those windows.
    """
    x = np.asarray(x, float)
    y = np.asarray(y, float)
    real = wcc(x, y, fs, window_s, step_s, max_lag_s)
    if not np.isfinite(real["peak_r"]).any():
        return None

    def zmean(r):
        r = r[np.isfinite(r)]
        return float(np.tanh(np.mean(np.arctanh(np.clip(r, -0.999, 0.999))))) if r.size else np.nan

    stat = zmean(real["peak_r"])
    n = min(len(x), len(y))
    margin = int(round((window_s + 2 * max_lag_s) * fs))
    rng = np.random.default_rng(seed)
    sur_stats, sur_peaks = [], []
    if n > 2 * margin:
        for shift in rng.integers(margin, n - margin, size=N_SURROGATES):
            s = wcc(x[:n], np.roll(y[:n], int(shift)), fs, window_s, step_s, max_lag_s)
            sur_stats.append(zmean(s["peak_r"]))
            sur_peaks.append(s["peak_r"][np.isfinite(s["peak_r"])])
    sur_stats = np.array([v for v in sur_stats if np.isfinite(v)])
    peaks = np.concatenate(sur_peaks) if sur_peaks else np.array([])
    p95 = float(np.percentile(peaks, 95)) if peaks.size else np.nan
    strong = np.isfinite(real["peak_r"]) & (real["peak_r"] > p95)
    lags = real["peak_lag_s"][strong]

    def fin(v, digits: int = 4):
        """Rounded, or None when not a number (JSON has no NaN)."""
        return round(float(v), digits) if v is not None and np.isfinite(v) else None

    out = {
        "mean_peak_r": fin(stat),
        "mean_lag0_r": fin(zmean(real["lag0_r"])),
        "windows": int(np.isfinite(real["peak_r"]).sum()),
        "surrogates": int(sur_stats.size),
        "surrogate_mean_peak_r": round(float(sur_stats.mean()), 4) if sur_stats.size else None,
        "surrogate_p95": fin(p95),
        "synchrony": round(stat - float(sur_stats.mean()), 4) if sur_stats.size else None,
        "p_value": round(float((1 + (sur_stats >= stat).sum()) / (1 + sur_stats.size)), 4)
        if sur_stats.size
        else None,
        "strong_windows": int(strong.sum()),
        "a_leads_pct": round(100.0 * float((lags > 0).mean()), 1) if lags.size else None,
        "b_leads_pct": round(100.0 * float((lags < 0).mean()), 1) if lags.size else None,
        "median_lag_s": round(float(np.median(lags)), 2) if lags.size else None,
    }

    def clean(v):
        return [None if not np.isfinite(e) else round(float(e), 4) for e in v]

    return {
        "params": {"window_s": window_s, "step_s": step_s, "max_lag_s": max_lag_s, "fs": fs},
        "summary": out,
        "series": {
            "t_s": [round(float(t), 3) for t in real["t_s"]],
            "peak_r": clean(real["peak_r"]),
            "peak_lag_s": clean(real["peak_lag_s"]),
        },
    }
