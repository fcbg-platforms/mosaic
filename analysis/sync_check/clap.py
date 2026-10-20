"""Claps heard and claps seen: is the audio placed right against the video?

MOSAIC's cameras are triggered together, so the videos agree with each other;
the audio is recorded apart and placed on the video clock afterwards (from the
recorder's timing file, else from the session start; see
:mod:`conversation.timing`). Every analysis that uses sound with images relies
on that placement. A clap checks it: one instant, both heard (a sharp sound)
and seen (two hands meeting). The audio time minus the video time of the same
clap is the offset still left in the placement.

Hearing a clap follows the onset detection of the 4by4k pipeline
(``fourk/synccheck.py``, MIT licence): spectral flux in a band above most of
the voice's energy, so a clap stands out from speech.
"""

from __future__ import annotations

from math import gcd

import numpy as np
from scipy.ndimage import median_filter
from scipy.signal import butter, resample_poly, sosfiltfilt

CLAP_BAND_HZ = (1500.0, 7000.0)
HOP_S = 0.004  # 4 ms steps
WIN_S = 0.025
#: Onset flux over its local median (the surrounding second): below, not a clap.
MIN_SHARPNESS = 6.0
#: An onset this close after a stronger one is its echo.
ECHO_S = 0.5
#: A seen clap: the wrists at most this far apart (body heights) ...
TOUCH = 0.12
#: ... after being at least this far apart within the previous half second.
APART = 0.3
APPROACH_S = 0.5
#: The audio may be placed this far off at most: the search for its offset.
MAX_LAG_S = 2.0
#: Offsets are voted in bins this wide; a seen clap then pairs with a heard
#: one within MATCH_S of that offset.
LAG_BIN_S = 0.02
MATCH_S = 0.15


#: Audio is brought to this rate first: claps' onsets need no more, and an
#: hour of audio stays small.
RATE = 16000


def onset_times(x: np.ndarray, fs: int) -> np.ndarray:
    """Seconds (from sample 0) of sharp onsets in ``x``: claps, knocks.

    Log-magnitude spectral flux in :data:`CLAP_BAND_HZ` (energy *starting*,
    not loud: a clap across the room counts like one nearby), peaks over
    :data:`MIN_SHARPNESS` times the flux's median in the second around them,
    the weaker of two within :data:`ECHO_S` dropped."""
    x = np.asarray(x, dtype=np.float32)
    if x.ndim > 1:
        x = x.mean(axis=1)
    if fs != RATE:
        g = gcd(int(fs), RATE)
        x = resample_poly(x, RATE // g, int(fs) // g).astype(np.float32)
        fs = RATE
    hop = int(round(HOP_S * fs))
    win = int(round(WIN_S * fs))
    n = (len(x) - win) // hop + 1
    if n < 3:
        return np.array([])
    freqs = np.fft.rfftfreq(win, 1.0 / fs)
    band = (freqs >= CLAP_BAND_HZ[0]) & (freqs <= CLAP_BAND_HZ[1])
    window = np.hanning(win).astype(np.float32)
    flux = np.empty(n - 1, dtype=np.float32)  # flux[i]: the step into frame i + 1
    prev = None
    for start in range(0, n, 4096):  # in blocks, keeping only the flux
        idx = np.arange(start, min(n, start + 4096))
        frames = x[idx[:, None] * hop + np.arange(win)[None, :]] * window
        mags = np.log1p(np.abs(np.fft.rfft(frames, axis=1))[:, band] * 100.0)
        if prev is not None:
            mags = np.vstack([prev[None, :], mags])
        d = np.maximum(np.diff(mags, axis=0), 0.0).sum(axis=1)
        lo = start if prev is None else start - 1
        flux[lo : lo + len(d)] = d
        prev = mags[-1]
    # The median over the second around each step, without copying windows
    # (an hour of audio is 900 000 steps).
    local = median_filter(flux, size=2 * int(round(0.5 / HOP_S)) + 1, mode="nearest") + 1e-9
    sharp = flux / local
    peaks = [
        i
        for i in range(1, len(flux) - 1)
        if sharp[i] >= MIN_SHARPNESS and flux[i] >= flux[i - 1] and flux[i] > flux[i + 1]
    ]
    peaks.sort(key=lambda i: -flux[i])
    kept: list = []
    for i in peaks:  # strongest first: an echo never outranks its clap
        if all(abs(i - j) * HOP_S > ECHO_S for j in kept):
            kept.append(i)
    # The flux peaks while the clap is somewhere in the 25 ms window, so the
    # frame's time is up to a window early; refine each on the samples.
    sos = butter(4, CLAP_BAND_HZ, btype="bandpass", fs=fs, output="sos")
    return np.sort([refine_onset(x, fs, (i + 1) * hop, win, sos) for i in kept])


def refine_onset(x: np.ndarray, fs: int, frame_start: int, win: int, sos) -> float:
    """The clap's start, seconds: in the clap band, the first sample whose
    level rises half way from the background before to the peak, searched
    from the frame's start to a window after its end."""
    pre = int(0.03 * fs)
    a = max(0, frame_start - pre)
    b = min(len(x), frame_start + 2 * win)
    seg = np.abs(sosfiltfilt(sos, x[a:b]))
    # A short running maximum, so one zero crossing does not count as quiet.
    k = max(1, int(0.0005 * fs))
    env = np.maximum.reduce([np.roll(seg, -j) for j in range(k)])
    start = frame_start - a
    background = float(np.median(env[:start])) if start > 0 else 0.0
    peak = float(env[start:].max()) if start < len(env) else 0.0
    if peak <= background:
        return frame_start / fs
    over = np.flatnonzero(env[start:] >= background + 0.5 * (peak - background))
    return (a + start + int(over[0])) / fs


def seen_claps(t_ms: np.ndarray, gap: np.ndarray) -> np.ndarray:
    """Times (ms) the hands met: ``gap`` (the wrists' distance in body heights,
    NaN where not seen) dips to :data:`TOUCH` or less, having been
    :data:`APART` or more within :data:`APPROACH_S` before. Each dip's time is
    refined between frames from the dip's shape (a parabola through the
    lowest frame and its neighbours)."""
    t = np.asarray(t_ms, dtype=np.float64)
    g = np.asarray(gap, dtype=np.float64)
    out = []
    for i in range(1, len(g) - 1):
        if not (np.isfinite(g[i]) and g[i] <= TOUCH):
            continue
        if not (g[i] <= g[i - 1] and g[i] < g[i + 1]):  # NaN neighbours fail too
            continue
        before = (t >= t[i] - APPROACH_S * 1000.0) & (t < t[i])
        if not (np.nan_to_num(g[before], nan=0.0) >= APART).any():
            continue
        a, b, c = g[i - 1], g[i], g[i + 1]
        denom = a - 2 * b + c
        shift = 0.5 * (a - c) / denom if denom > 0 else 0.0
        shift = float(np.clip(shift, -0.5, 0.5))
        dt = (t[i + 1] - t[i - 1]) / 2.0
        out.append(t[i] + shift * dt)
    return np.asarray(out)


def estimate_lag(heard_ms: np.ndarray, seen_ms: np.ndarray) -> float | None:
    """The audio's offset from the video, ms: the heard-minus-seen difference
    (within :data:`MAX_LAG_S`) that the most seen claps agree on, in bins of
    :data:`LAG_BIN_S`; ties go to the smaller offset. Found over the whole
    pattern of claps first, so a regular clapping cannot pair each clap with
    its neighbour; ``None`` without a candidate."""
    heard = np.asarray(heard_ms, dtype=np.float64)
    seen = np.asarray(seen_ms, dtype=np.float64)
    votes: dict = {}
    for k, s in enumerate(seen):
        d = heard - s
        for b in {
            int(np.round(x / (LAG_BIN_S * 1000.0))) for x in d[np.abs(d) <= MAX_LAG_S * 1000.0]
        }:
            votes.setdefault(b, set()).add(k)
    if not votes:
        return None
    # A difference on a bin edge counts for the neighbour too.
    support = {
        b: len(votes.get(b, set()) | votes.get(b - 1, set()) | votes.get(b + 1, set()))
        for b in votes
    }
    best = max(support, key=lambda b: (support[b], -abs(b)))
    return best * LAG_BIN_S * 1000.0


def match(heard_ms: np.ndarray, seen_ms: np.ndarray) -> list:
    """Each seen clap with the heard one nearest the estimated offset
    (:func:`estimate_lag`), within :data:`MATCH_S` of it, each heard clap used
    once: ``(seen_ms, heard_ms, heard - seen)``."""
    lag = estimate_lag(heard_ms, seen_ms)
    if lag is None:
        return []
    heard = list(np.sort(np.asarray(heard_ms, dtype=np.float64)))
    pairs = []
    for s in np.sort(np.asarray(seen_ms, dtype=np.float64)):
        if not heard:
            break
        j = int(np.argmin([abs(h - s - lag) for h in heard]))
        if abs(heard[j] - s - lag) <= MATCH_S * 1000.0:
            pairs.append((float(s), float(heard[j]), float(heard[j] - s)))
            heard.pop(j)
    return pairs


def verdict(offsets_ms: list, frame_ms: float) -> str:
    """``ok`` within half a frame, ``check`` within a frame, ``off`` beyond
    (the median offset); ``none`` without a matched clap."""
    if not offsets_ms:
        return "none"
    m = abs(float(np.median(offsets_ms)))
    return "ok" if m <= frame_ms / 2.0 else "check" if m <= frame_ms else "off"
