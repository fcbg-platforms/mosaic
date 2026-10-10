"""
Beat-to-beat heart timing and heart-rate variability (HRV) from a face
video's colour signal.

The windowed heart rate in :mod:`rppg.hr_estimation` finds the dominant
frequency of 10 s stretches; HRV needs every beat. The steps:

1. **Uniform resampling** (:func:`resample_uniform`): the per-frame skin
   colour, placed at each frame's real timestamp, is resampled onto an even
   grid; gaps up to :data:`MAX_GAP_S` are bridged, longer ones split the
   recording into segments analysed separately.
2. **Continuous pulse wave** (:func:`pos_overlap_add`): the POS projection
   (Wang et al., 2017) on short overlapping windows of
   :data:`POS_WINDOW_S`, added back together as in the original method, so
   the wave follows slow changes of light and skin tone.
3. **Heart rate and band**: the dominant frequency in 0.7 to 3 Hz gives the
   typical beat interval; the beats are timed on a wider band
   (:data:`BEAT_BAND`) that keeps each beat's shape.
4. **Beats** (:func:`detect_beats`): systolic peaks on a 4x upsampled wave,
   at least 0.6 of the typical beat interval apart; each beat is timed at
   the steepest point of its upstroke, refined to sub-sample time with a
   parabola.
   Each beat is then re-timed by matching its whole shape against the
   person's average beat (:func:`refine_with_template`), which is far less
   sensitive to noise than any single point of the wave.
5. **Beat quality** (:func:`beat_quality`): each beat's correlation with the
   person's average beat; poorly shaped beats are not used.
6. **Cleaning** (:func:`clean_intervals`): intervals outside 333 to 1500 ms
   (40 to 180 bpm), or more than :data:`MAX_DEVIATION` from the local
   median, are artifacts; only intervals between two good beats in the same
   segment become **NN intervals**.
7. **HRV** (:func:`hrv_time_domain`, :func:`hrv_frequency_domain`): the
   Task Force (1996) measures. RMSSD and pNN50 reflect beat-to-beat
   (vagal) variation; SDNN overall variability; LF and HF power from the
   spectrum of the NN series resampled at 4 Hz.

Timing noise matters: a camera at 50 fps samples every 20 ms, and noise
moves each beat by several to tens of milliseconds. Noise of sigma per beat
adds 6 sigma^2 to RMSSD^2 and 2 sigma^2 to SDNN^2, so rPPG **overestimates**
both, most at low true variability. The two halves of the face give two
independent views of the same beats: their timing differences measure sigma
(:func:`timing_jitter`), and the corrected values take that variance out.
See :doc:`/math/remote_heart_rate`.
"""

from __future__ import annotations

import math

import numpy as np

#: Gaps in the face track bridged by interpolation, seconds.
MAX_GAP_S = 0.5
#: Segments shorter than this are not analysed, seconds.
MIN_SEGMENT_S = 10.0
#: POS window (Wang et al. use 1.6 s at 20 fps), seconds.
POS_WINDOW_S = 1.6
#: Physiological band, where the heart rate is looked for, Hz.
LOW_HZ, HIGH_HZ = 0.7, 3.0
#: Band for timing beats: wide enough to keep the upstroke sharp, Hz.
BEAT_BAND = (0.5, 5.0)
#: Upsampling for peak timing.
UPSAMPLE = 4
#: Beats whose shape correlates with the average beat below this are poor.
MIN_BEAT_CORRELATION = 0.6
#: Interval limits and the largest change from the local median.
MIN_IBI_S, MAX_IBI_S = 60.0 / 180.0, 60.0 / 40.0
MAX_DEVIATION = 0.2
LOCAL_MEDIAN_BEATS = 11
#: Matched beats needed between the two face halves to estimate timing noise.
MIN_JITTER_PAIRS = 30
#: A sliding window reports heart rate and RMSSD from this many NN intervals.
MIN_WINDOW_NN = 30
#: HRV needs at least this much clean beat-to-beat time, seconds, and at
#: most this share of artifacts.
MIN_NN_S = 60.0
MAX_ARTIFACT_SHARE = 0.25
#: Below this frame rate beat timing is too coarse for HRV.
MIN_FPS_FOR_HRV = 25.0
#: Frequency bands, Hz (Task Force, 1996).
LF_BAND = (0.04, 0.15)
HF_BAND = (0.15, 0.40)
#: Resampling rate of the NN series for its spectrum, Hz.
SPECTRUM_FS = 4.0


# ── Resampling and the pulse wave ────────────────────────────────────────────


def resample_uniform(times_s, rgb, fs: float) -> list[tuple[np.ndarray, np.ndarray]]:
    """Per-frame colour onto an even grid, as segments.

    Parameters
    ----------
    times_s : array_like
        Frame times, seconds (ascending; frames without a face omitted).
    rgb : array_like
        ``(N, 3)`` mean skin colour per frame.
    fs : float
        Grid rate, Hz (the camera's frame rate).

    Returns
    -------
    list of (times, rgb)
        One entry per stretch without a gap over :data:`MAX_GAP_S`, each at
        least :data:`MIN_SEGMENT_S` long.
    """
    t = np.asarray(times_s, dtype=np.float64)
    x = np.asarray(rgb, dtype=np.float64).reshape(-1, 3)
    if t.size < 2:
        return []
    cuts = np.flatnonzero(np.diff(t) > MAX_GAP_S) + 1
    out = []
    for a, b in zip(np.concatenate([[0], cuts]), np.concatenate([cuts, [t.size]]), strict=True):
        ts, xs = t[a:b], x[a:b]
        if ts.size < 2 or ts[-1] - ts[0] < MIN_SEGMENT_S:
            continue
        grid = np.arange(ts[0], ts[-1], 1.0 / fs)
        out.append((grid, np.column_stack([np.interp(grid, ts, xs[:, k]) for k in range(3)])))
    return out


def pos_overlap_add(rgb, fs: float, window_s: float = POS_WINDOW_S) -> np.ndarray:
    """POS pulse wave over a whole segment (Wang et al., 2017, Algorithm 1):
    each window of ``window_s`` is normalised by its own mean, projected onto
    the plane orthogonal to skin tone, tuned with ``alpha = std(S1)/std(S2)``,
    zero-meaned and added into the output."""
    x = np.asarray(rgb, dtype=np.float64)
    n = len(x)
    w = max(2, int(round(window_s * fs)))
    h = np.zeros(n)
    if n < w:
        return h
    proj = np.array([[0.0, 1.0, -1.0], [-2.0, 1.0, 1.0]])
    for m in range(0, n - w + 1):
        c = x[m : m + w]
        mean = c.mean(axis=0)
        if np.any(mean <= 0):
            continue
        s = (c / mean) @ proj.T
        sd = s[:, 1].std()
        p = s[:, 0] + (s[:, 0].std() / sd if sd > 1e-12 else 0.0) * s[:, 1]
        h[m : m + w] += p - p.mean()
    return h


def dominant_hz(signal, fs: float, low: float = LOW_HZ, high: float = HIGH_HZ) -> float | None:
    """Frequency of the strongest peak in ``[low, high]`` (Welch)."""
    from scipy.signal import welch

    x = np.asarray(signal, dtype=np.float64)
    if x.size < int(4 * fs):
        return None
    f, p = welch(x, fs=fs, nperseg=min(x.size, int(16 * fs)))
    band = (f >= low) & (f <= high)
    if not band.any() or p[band].max() <= 0:
        return None
    return float(f[band][np.argmax(p[band])])


def bandpass(signal, fs: float, low: float, high: float) -> np.ndarray:
    """Zero-phase 3rd-order Butterworth band-pass (no shift of beat times)."""
    from scipy.signal import butter, sosfiltfilt

    sos = butter(3, [low, high], btype="band", fs=fs, output="sos")
    return sosfiltfilt(sos, np.asarray(signal, dtype=np.float64))


# ── Beats ────────────────────────────────────────────────────────────────────


def detect_beats(pulse, times_s, heart_hz: float | None) -> np.ndarray:
    """Beat times (seconds), each at the steepest point of the pulse
    upstroke.

    The wave is upsampled :data:`UPSAMPLE` times with a cubic spline;
    systolic peaks at least 0.6 of the typical interval apart and standing
    out against the local amplitude are found first, then each beat is timed
    at the maximum slope in the stretch before its peak, refined with a
    parabola. The upstroke is the standard fiducial point for pulse waves:
    sharper, and less shifted by the wave's shape, than the rounded peak.
    """
    from scipy.interpolate import CubicSpline
    from scipy.signal import find_peaks

    x = np.asarray(pulse, dtype=np.float64)
    t = np.asarray(times_s, dtype=np.float64)
    if x.size < 4:
        return np.zeros(0)
    fs = (t.size - 1) / (t[-1] - t[0])
    up = np.linspace(t[0], t[-1], (t.size - 1) * UPSAMPLE + 1)
    spline = CubicSpline(t, x)
    y = spline(up)
    slope = spline(up, 1)
    fs_up = fs * UPSAMPLE
    ibi = 1.0 / heart_hz if heart_hz else 0.8
    scale = np.percentile(np.abs(y), 90) if y.size else 0.0
    peaks, _ = find_peaks(y, distance=max(1, int(0.6 * ibi * fs_up)), prominence=0.3 * scale)
    reach = max(2, int(0.4 * ibi * fs_up))
    out = []
    for p in peaks:
        lo = max(1, p - reach)
        if p - lo < 2:
            continue
        k = lo + int(np.argmax(slope[lo:p]))
        if not 0 < k < slope.size - 1:
            continue
        a, b, c = slope[k - 1], slope[k], slope[k + 1]
        den = a - 2 * b + c
        shift = 0.5 * (a - c) / den if abs(den) > 1e-12 else 0.0
        out.append(up[k] + float(np.clip(shift, -0.5, 0.5)) / fs_up)
    return np.asarray(out)


def refine_with_template(pulse, times_s, beats_s, search_s: float = 0.08) -> np.ndarray:
    """Re-time every beat by matching its whole shape to the average beat.

    A single point of a noisy wave (the upstroke) wobbles by tens of
    milliseconds; cross-correlating the stretch around each beat with the
    person's median beat uses every sample of it (a matched filter, the
    best timing estimate under white noise). Shifts within ``search_s`` are
    tried on a 1 ms grid and the best is refined with a parabola.
    """
    x = np.asarray(pulse, dtype=np.float64)
    t = np.asarray(times_s, dtype=np.float64)
    b = np.asarray(beats_s, dtype=np.float64)
    if b.size < 5:
        return b
    ibi = float(np.median(np.diff(b)))
    offsets = np.arange(-0.3 * ibi, 0.6 * ibi, 0.004)
    shapes = np.array([np.interp(bt + offsets, t, x, left=np.nan, right=np.nan) for bt in b])
    ok = np.all(np.isfinite(shapes), axis=1)
    if ok.sum() < 5:
        return b
    template = np.median(shapes[ok], axis=0)
    template = template - template.mean()
    shifts = np.arange(-search_s, search_s + 1e-9, 0.001)
    out = b.copy()
    for i in np.flatnonzero(ok):
        seg = np.array([np.interp(b[i] + sh + offsets, t, x) for sh in shifts])
        seg = seg - seg.mean(axis=1, keepdims=True)
        score = seg @ template / (np.linalg.norm(seg, axis=1) * np.linalg.norm(template) + 1e-12)
        k = int(np.argmax(score))
        if 0 < k < score.size - 1:
            a_, b_, c_ = score[k - 1], score[k], score[k + 1]
            den = a_ - 2 * b_ + c_
            frac = 0.5 * (a_ - c_) / den if abs(den) > 1e-12 else 0.0
            out[i] = b[i] + shifts[k] + float(np.clip(frac, -0.5, 0.5)) * 0.001
        else:
            out[i] = b[i] + shifts[k]
    return out


def beat_quality(pulse, times_s, beats_s) -> np.ndarray:
    """Correlation of each beat's shape (one median interval centred on it)
    with the average beat."""
    x = np.asarray(pulse, dtype=np.float64)
    t = np.asarray(times_s, dtype=np.float64)
    b = np.asarray(beats_s, dtype=np.float64)
    if b.size < 3:
        return np.zeros(b.size)
    half = 0.5 * float(np.median(np.diff(b)))
    offsets = np.linspace(-half, half, 32)
    shapes = np.array([np.interp(bt + offsets, t, x, left=np.nan, right=np.nan) for bt in b])
    ok = np.all(np.isfinite(shapes), axis=1)
    q = np.zeros(b.size)
    if ok.sum() < 3:
        return q
    template = np.median(shapes[ok], axis=0)
    template = (template - template.mean()) / (template.std() + 1e-12)
    for i in np.flatnonzero(ok):
        s = shapes[i]
        s = (s - s.mean()) / (s.std() + 1e-12)
        q[i] = float(np.mean(s * template))
    return q


def clean_intervals(beats_s, quality, segment_ids) -> dict:
    """Beat-to-beat intervals and which of them are NN (normal-to-normal).

    Returns
    -------
    dict
        ``ibi_s`` (each interval, at the second beat's time ``t_s``),
        ``nn`` (bool per interval), and the counts.
    """
    b = np.asarray(beats_s, dtype=np.float64)
    q = np.asarray(quality, dtype=np.float64)
    seg = np.asarray(segment_ids)
    if b.size < 2:
        return {"t_s": np.zeros(0), "ibi_s": np.zeros(0), "nn": np.zeros(0, bool)}
    ibi = np.diff(b)
    t = b[1:]
    same = seg[1:] == seg[:-1]
    good = same & (q[1:] >= MIN_BEAT_CORRELATION) & (q[:-1] >= MIN_BEAT_CORRELATION)
    plausible = (ibi >= MIN_IBI_S) & (ibi <= MAX_IBI_S)
    ref = np.full(ibi.size, np.nan)
    half = LOCAL_MEDIAN_BEATS // 2
    cand = good & plausible
    for i in range(ibi.size):
        lo, hi = max(0, i - half), min(ibi.size, i + half + 1)
        w = ibi[lo:hi][cand[lo:hi]]
        if w.size >= 3:
            ref[i] = np.median(w)
    steady = np.isfinite(ref) & (np.abs(ibi - ref) <= MAX_DEVIATION * ref)
    nn = cand & steady
    return {"t_s": t, "ibi_s": ibi, "nn": nn, "same_segment": same}


# ── HRV ──────────────────────────────────────────────────────────────────────


def _consecutive_nn_diffs(ibi_s, nn) -> np.ndarray:
    """Differences between successive intervals that are both NN."""
    ibi = np.asarray(ibi_s, dtype=np.float64)
    nn = np.asarray(nn, dtype=bool)
    both = nn[1:] & nn[:-1]
    return np.diff(ibi)[both]


def hrv_time_domain(ibi_s, nn) -> dict:
    """Mean NN, heart rate, SDNN, RMSSD and pNN50 (milliseconds and bpm)."""
    ibi = np.asarray(ibi_s, dtype=np.float64)
    nn = np.asarray(nn, dtype=bool)
    v = ibi[nn]
    d = _consecutive_nn_diffs(ibi, nn)
    if v.size < 2:
        return {"nn_count": int(v.size)}
    out = {
        "nn_count": int(v.size),
        "mean_nn_ms": round(1000.0 * float(v.mean()), 1),
        "mean_hr_bpm": round(60.0 / float(v.mean()), 1),
        "sdnn_ms": round(1000.0 * float(v.std(ddof=1)), 1),
    }
    if d.size >= 2:
        out["rmssd_ms"] = round(1000.0 * float(np.sqrt(np.mean(d**2))), 1)
        out["pnn50_pct"] = round(100.0 * float(np.mean(np.abs(d) > 0.05)), 1)
    return out


def hrv_frequency_domain(t_s, ibi_s, nn) -> dict:
    """LF and HF power (ms²), LF/HF and the HF peak frequency.

    The usual method (as in Kubios): the NN intervals, with artifacts
    bridged by interpolation, are resampled at :data:`SPECTRUM_FS` Hz by a
    cubic spline, detrended, and Welch's periodogram (64 s segments) is
    integrated over each band. Needs at least 2 minutes.
    """
    from scipy.interpolate import CubicSpline
    from scipy.signal import detrend, welch

    keep = np.asarray(nn, bool)
    t = np.asarray(t_s, dtype=np.float64)[keep]
    v = 1000.0 * np.asarray(ibi_s, dtype=np.float64)[keep]
    if t.size < 20 or t[-1] - t[0] < 120.0:
        return {}
    grid = np.arange(t[0], t[-1], 1.0 / SPECTRUM_FS)
    x = detrend(CubicSpline(t, v)(grid))
    f, psd = welch(x, fs=SPECTRUM_FS, nperseg=min(x.size, int(64 * SPECTRUM_FS)))
    df = f[1] - f[0]

    def power(band):
        sel = (f >= band[0]) & (f < band[1])
        return float(np.sum(psd[sel]) * df)

    lf, hf = power(LF_BAND), power(HF_BAND)
    out = {"lf_ms2": round(lf, 1), "hf_ms2": round(hf, 1)}
    if hf > 0:
        out["lf_hf"] = round(lf / hf, 2)
    hf_sel = (f >= HF_BAND[0]) & (f < HF_BAND[1])
    if hf_sel.any():
        out["hf_peak_hz"] = round(float(f[hf_sel][np.argmax(psd[hf_sel])]), 3)
    return out


def windowed_hrv(t_s, ibi_s, nn, window_s: float = 60.0, hop_s: float = 10.0) -> list[dict]:
    """Heart rate and RMSSD in sliding windows (for the chart)."""
    t = np.asarray(t_s, dtype=np.float64)
    if t.size == 0:
        return []
    out = []
    start = float(t[0])
    while start + window_s <= float(t[-1]) + 1e-9:
        sel = (t >= start) & (t < start + window_s)
        td = hrv_time_domain(np.asarray(ibi_s)[sel], np.asarray(nn)[sel])
        enough = td.get("nn_count", 0) >= MIN_WINDOW_NN
        out.append(
            {
                "start_s": start,
                "end_s": start + window_s,
                "hr_bpm": td.get("mean_hr_bpm") if enough else None,
                "rmssd_ms": td.get("rmssd_ms") if enough else None,
                "nn_count": td.get("nn_count", 0),
            }
        )
        start += hop_s
    return out


def _beats(times_s, rgb, fs: float):
    """Beats of one colour signal: times, quality, segment ids, the heart
    rate of each segment and the segments' spans."""
    segments = resample_uniform(times_s, rgb, fs)
    beats, quality, seg_ids, rates = [], [], [], []
    for k, (grid, colour) in enumerate(segments):
        pulse = pos_overlap_add(colour, fs)
        hz = dominant_hz(bandpass(pulse, fs, LOW_HZ, HIGH_HZ), fs)
        # The heart rate comes from the physiological band; the beats are
        # timed on a wider one, which keeps each beat's shape and its own
        # timing (a narrow band turns the wave into a sinusoid that cannot
        # follow beat-to-beat changes).
        shape = bandpass(pulse, fs, BEAT_BAND[0], min(BEAT_BAND[1], 0.45 * fs))
        b = refine_with_template(shape, grid, detect_beats(shape, grid, hz))
        beats.append(b)
        quality.append(beat_quality(shape, grid, b))
        seg_ids.append(np.full(b.size, k))
        if hz:
            rates.append(hz * 60.0)
    spans = [(float(g[0]), float(g[-1])) for g, _ in segments]
    if not beats:
        return np.zeros(0), np.zeros(0), np.zeros(0, int), rates, spans
    return np.concatenate(beats), np.concatenate(quality), np.concatenate(seg_ids), rates, spans


def timing_jitter(beats_a, quality_a, beats_b, quality_b) -> tuple[float | None, int]:
    """Beat-timing noise of the whole face from two independent halves.

    Both halves see the same heartbeat, so their beat times differ only by
    noise (and a constant offset). With independent noise of standard
    deviation s in each half, the difference has sqrt(2) s, and the whole
    face, averaging both, s / sqrt(2): so the whole face's jitter is half the
    spread of the differences. The spread is taken robustly (1.4826 times the
    median absolute deviation).

    Returns ``(jitter_s, pairs)``; ``None`` with fewer than
    :data:`MIN_JITTER_PAIRS` matched good beats.
    """
    a = np.asarray(beats_a, float)[np.asarray(quality_a) >= MIN_BEAT_CORRELATION]
    b = np.asarray(beats_b, float)[np.asarray(quality_b) >= MIN_BEAT_CORRELATION]
    if a.size == 0 or b.size < 2:
        return None, 0
    j = np.clip(np.searchsorted(b, a), 1, b.size - 1)
    nearest = np.where(np.abs(b[j - 1] - a) < np.abs(b[j] - a), b[j - 1], b[j])
    d = nearest - a
    d = d[np.abs(d - np.median(d)) < 0.1]
    if d.size < MIN_JITTER_PAIRS:
        return None, int(d.size)
    mad = float(np.median(np.abs(d - np.median(d))))
    return 1.4826 * mad / 2.0, int(d.size)


def corrected(measured_ms: float | None, jitter_ms: float, factor: float) -> float | None:
    """A variability measure with the variance timing noise adds taken out:
    ``sqrt(max(measured² - factor * jitter², 0))``."""
    if measured_ms is None:
        return None
    return round(math.sqrt(max(measured_ms**2 - factor * jitter_ms**2, 0.0)), 1)


def analyse(times_s, rgb, fs: float, halves=None) -> dict:
    """The whole chain on one recording's per-frame colour.

    ``halves`` (optional) is the colour of the two halves of the face,
    ``(left, right)`` arrays like ``rgb`` (rows of ``nan`` where a half was
    not measured); they give the timing noise and noise-corrected RMSSD and
    SDNN.

    Returns beats (time, quality), intervals (time, length, NN), HRV, the
    windowed series and the reasons HRV is withheld, if it is.
    """
    if fs < MIN_FPS_FOR_HRV:
        # Too coarse for beat timing (and below 2 x 3 Hz the band-pass
        # itself is impossible): no beats at all.
        return {
            "beats_s": np.zeros(0),
            "beat_quality": np.zeros(0),
            "ibi": {"t_s": np.zeros(0), "ibi_s": np.zeros(0), "nn": np.zeros(0, bool)},
            "segments": [],
            "nn_seconds": 0.0,
            "artifact_share": 1.0,
            "hrv": None,
            "hrv_withheld": [f"frame rate {fs:.0f} fps is below {MIN_FPS_FOR_HRV:.0f} fps"],
            "timing_jitter_s": None,
            "jitter_pairs": 0,
            "windows": [],
            "segment_hr_bpm": [],
            "timing_note": _timing_note(fs),
        }
    b, q, s, rates, spans = _beats(times_s, rgb, fs)
    iv = clean_intervals(b, q, s)
    nn = iv["nn"]
    nn_s = float(np.sum(iv["ibi_s"][nn])) if nn.size else 0.0
    usable = iv.get("same_segment", np.zeros(0, bool))
    artifacts = float(1.0 - nn.sum() / usable.sum()) if usable.sum() else 1.0
    reasons = []
    if nn_s < MIN_NN_S:
        reasons.append(f"only {nn_s:.0f} s of clean beats (needs {MIN_NN_S:.0f} s)")
    if artifacts > MAX_ARTIFACT_SHARE:
        reasons.append(
            f"{100 * artifacts:.0f}% of intervals are artifacts "
            f"(limit {100 * MAX_ARTIFACT_SHARE:.0f}%)"
        )
    jitter, pairs = None, 0
    if halves is not None:
        t = np.asarray(times_s, dtype=np.float64)
        sides = []
        for h in halves:
            h = np.asarray(h, dtype=np.float64)
            ok = np.all(np.isfinite(h), axis=1)
            sides.append(_beats(t[ok], h[ok], fs)[:2])
        jitter, pairs = timing_jitter(sides[0][0], sides[0][1], sides[1][0], sides[1][1])
    hrv = None
    if not reasons:
        hrv = hrv_time_domain(iv["ibi_s"], nn)
        hrv.update(hrv_frequency_domain(iv["t_s"], iv["ibi_s"], nn))
        if jitter is not None:
            j_ms = 1000.0 * jitter
            hrv["timing_jitter_ms"] = round(j_ms, 1)
            # Per-beat noise s adds 6 s² to RMSSD² (a successive difference
            # of intervals is b[k+1] - 2 b[k] + b[k-1]) and 2 s² to SDNN².
            hrv["rmssd_corrected_ms"] = corrected(hrv.get("rmssd_ms"), j_ms, 6.0)
            hrv["sdnn_corrected_ms"] = corrected(hrv.get("sdnn_ms"), j_ms, 2.0)
    return {
        "beats_s": b,
        "beat_quality": q,
        "ibi": iv,
        "segments": spans,
        "nn_seconds": nn_s,
        "artifact_share": artifacts,
        "hrv": hrv,
        "hrv_withheld": reasons,
        "timing_jitter_s": jitter,
        "jitter_pairs": pairs,
        # The sliding-window series only when the recording's HRV holds up.
        "windows": windowed_hrv(iv["t_s"], iv["ibi_s"], nn) if hrv is not None else [],
        "segment_hr_bpm": [round(r, 1) for r in rates],
        "timing_note": _timing_note(fs),
    }


def _timing_note(fs: float) -> str:
    """How finely a frame rate samples the pulse, for the log."""
    return f"{1000.0 / fs:.0f} ms between frames, beat times refined below that by interpolation"


def rmssd_noise_floor_ms(jitter_ms: float) -> float:
    """RMSSD that pure, independent beat-timing jitter of ``jitter_ms``
    alone produces: a successive difference of intervals is
    ``b[k+1] - 2 b[k] + b[k-1]``, with standard deviation ``sqrt(6)`` times
    the jitter."""
    return math.sqrt(6.0) * jitter_ms
