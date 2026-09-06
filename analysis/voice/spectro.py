"""Pure numeric helpers for acoustic analysis — numpy only.

Deliberately free of parselmouth, file I/O and argument parsing, so every
decision that shapes what the operator finally sees can be tested without
installing Praat bindings or decoding audio. Mirrors ``analysis/rppg/algorithms.py``.

The functions here carry the choices that are wrong-but-plausible if you get
them backwards: which axis is reduced with which operator, whether averaging
happens in dB or in power, and when a pitch reading is real rather than the
tracker latching onto room tone.
"""

from __future__ import annotations

import numpy as np

# A spectrogram column is only ever a few screen pixels wide, so resolution
# beyond ~20 columns per second buys nothing visible and costs linearly in file
# size, parse time and paint time.
DEFAULT_COLS_PER_SECOND = 20.0
MIN_COLUMNS = 512
MAX_COLUMNS = 4096

# Rows always span 0 Hz to the analysis ceiling. A power of two, comfortably
# above any realistic widget height, and far finer than the ~260 Hz bandwidth of
# the 5 ms analysis window — so the limit on detail is the physics, not this.
SPECTROGRAM_ROWS = 256

# Praat's own broadband default. Resolves voicing striations and formants
# rather than individual harmonics, which is what makes speech legible.
WINDOW_LENGTH_S = 0.005


def target_columns(
    duration_s: float,
    *,
    cols_per_second: float = DEFAULT_COLS_PER_SECOND,
    min_cols: int = MIN_COLUMNS,
    max_cols: int = MAX_COLUMNS,
) -> int:
    """Columns to render for a clip of ``duration_s``, clamped to a fixed band.

    The upper clamp is the point: a one-hour recording produces the same
    4096-column image as a three-minute one, so loading and drawing cost the
    same regardless of length. What degrades with duration is time resolution,
    which is the right thing to trade.
    """
    if duration_s <= 0:
        return min_cols
    return int(np.clip(np.ceil(duration_s * cols_per_second), min_cols, max_cols))


def spectrogram_time_step(duration_s: float) -> float:
    """Praat analysis step to use for a clip of this length, in seconds.

    Escalates with duration because the output column budget is fixed: past ten
    minutes each column already averages hundreds of source frames, so analysing
    at 2 ms is pure waste — the extra frames are reduced away unseen.
    """
    if duration_s <= 600:
        return 0.002
    if duration_s <= 1800:
        return 0.004
    return 0.008


def track_step_ms(
    duration_s: float, *, base_dt_ms: float = 10.0, max_frames: int = 60_000
) -> float:
    """Storage step for the pitch/intensity tracks, in milliseconds.

    Analysis always runs at ``base_dt_ms`` — the tracker needs that resolution
    to work — but a long recording is decimated before being written, so the
    JSON stays a few hundred kilobytes instead of megabytes. Always a whole
    multiple of the base step, so decimation is a clean block reduction.
    """
    if duration_s <= 0:
        return base_dt_ms
    frames = duration_s * 1000.0 / base_dt_ms
    factor = max(1, int(np.ceil(frames / max_frames)))
    return base_dt_ms * factor


def column_indices(times_s: np.ndarray, n_cols: int, t0_s: float, t1_s: float) -> np.ndarray:
    """Output column each source frame belongs to, clamped to ``[0, n_cols)``."""
    if n_cols <= 0 or times_s.size == 0 or t1_s <= t0_s:
        return np.zeros(times_s.size, dtype=np.int64)
    idx = np.floor((times_s - t0_s) / (t1_s - t0_s) * n_cols).astype(np.int64)
    return np.clip(idx, 0, n_cols - 1)


def accumulate_column_max(
    out_db: np.ndarray, filled: np.ndarray, db: np.ndarray, cols: np.ndarray
) -> None:
    """Fold one chunk's dB frames into the output grid in place, taking the max.

    Streaming rather than concatenate-then-reduce, and that is the entire point.
    Holding every chunk's full-resolution slice and joining them at the end
    rebuilds exactly the whole-file matrix the chunking exists to avoid —
    measured at 2.8 GB peak for a 30-minute recording, against 13 MB for this
    reduced grid. Chunking Praat's input bounds *its* allocation; only reducing
    as we go bounds ours.

    Max rather than mean, in the dB domain: when one column spans most of a
    second, a mean smears every plosive into the silence around it and the
    result looks like a smudge. The max keeps the loudest thing that happened,
    which is what an audio editor shows and what a reader expects.
    """
    if db.size == 0 or cols.size == 0:
        return
    for c in np.unique(cols):
        ci = int(c)
        column = db[:, cols == c].max(axis=1)
        if filled[ci]:
            np.maximum(out_db[:, ci], column, out=out_db[:, ci])
        else:
            out_db[:, ci] = column
            filled[ci] = True


def fill_empty_columns(out_db: np.ndarray, filled: np.ndarray) -> None:
    """Carry a real column into any that caught no source frame.

    A hole would render as a black stripe through the spectrogram, which reads
    as silence rather than as "no sample landed in this column".
    """
    if out_db.size == 0 or filled.size == 0 or not filled.any():
        return
    first = int(np.argmax(filled))
    for i in range(first):
        out_db[:, i] = out_db[:, first]  # leading gap: back-fill
    for i in range(first + 1, filled.size):
        if not filled[i]:
            out_db[:, i] = out_db[:, i - 1]


def reduce_freq_mean(power: np.ndarray, n_rows: int) -> np.ndarray:
    """Band-average ``(n_freq, n_time)`` **power** down to ``n_rows`` rows.

    Averaging must happen in the power domain, not in dB. Power is what adds:
    averaging decibels averages logarithms, which biases every band toward its
    quietest bin and visibly darkens the image. Callers convert to dB after.
    """
    if power.size == 0 or n_rows <= 0:
        return np.zeros((max(n_rows, 0), power.shape[1] if power.ndim == 2 else 0))
    n_src = power.shape[0]
    if n_src <= n_rows:
        return power.astype(np.float64, copy=True)

    edges = np.linspace(0, n_src, n_rows + 1).astype(np.int64)
    out = np.empty((n_rows, power.shape[1]), dtype=np.float64)
    for i in range(n_rows):
        lo, hi = int(edges[i]), int(max(edges[i + 1], edges[i] + 1))
        out[i] = power[lo:hi].mean(axis=0)
    return out


def dynamic_range(
    db: np.ndarray, *, top_db: float = 70.0, hi_percentile: float = 99.9
) -> tuple[float, float]:
    """(floor_db, ceil_db) to map onto the image's 0..255.

    The ceiling is a high percentile rather than the maximum, so one clipped
    sample or a single door slam cannot compress the entire recording into the
    bottom of the range. 70 dB below that is roughly the useful span of speech
    against room tone.
    """
    if db.size == 0:
        return (0.0, 1.0)
    ceil = float(np.percentile(db, hi_percentile))
    return (ceil - top_db, ceil)


def db_to_uint8(db: np.ndarray, floor_db: float, ceil_db: float) -> np.ndarray:
    """Quantise dB to 0..255 for storage as an 8-bit greyscale PNG.

    Always applied *after* any reduction: quantising first throws away precision
    and then lets the reducer amplify the resulting noise. 8 bits over a 70 dB
    span is 0.27 dB per level, far below anything visible in a heatmap.
    """
    if db.size == 0:
        return np.zeros_like(db, dtype=np.uint8)
    span = max(ceil_db - floor_db, 1e-9)
    scaled = (np.nan_to_num(db, nan=floor_db) - floor_db) / span
    return (np.clip(scaled, 0.0, 1.0) * 255.0).round().astype(np.uint8)


def suppress_low_intensity_pitch(
    pitch_hz: np.ndarray,
    intensity_db: np.ndarray,
    *,
    drop_below_db: float = 35.0,
    ref_percentile: float = 95.0,
) -> np.ndarray:
    """Zero pitch readings from frames far quieter than the recording itself.

    Praat's tracker will happily find periodicity in HVAC hum and room tone, and
    those readings look exactly like speech on the plot. Anything more than
    ``drop_below_db`` under the file's own loud level is not someone talking.
    """
    out = np.array(pitch_hz, dtype=np.float64, copy=True)
    if out.size == 0 or intensity_db.size == 0:
        return out
    n = min(out.size, intensity_db.size)
    reference = float(np.percentile(intensity_db[:n], ref_percentile))
    quiet = intensity_db[:n] < (reference - drop_below_db)
    out[:n][quiet] = 0.0
    return out


def drop_short_voiced_runs(pitch_hz: np.ndarray, *, min_frames: int = 3) -> np.ndarray:
    """Zero voiced runs shorter than ``min_frames``.

    At the usual 10 ms step that is 30 ms — shorter than any real phonation, so
    such runs are tracker noise. They matter out of proportion to their length
    because each one puts an isolated dot on the plot.
    """
    out = np.array(pitch_hz, dtype=np.float64, copy=True)
    if out.size == 0 or min_frames <= 1:
        return out

    voiced = out > 0
    start = None
    for i, v in enumerate(np.append(voiced, False)):
        if v and start is None:
            start = i
        elif not v and start is not None:
            if i - start < min_frames:
                out[start:i] = 0.0
            start = None
    return out


def decimate_track(values: np.ndarray, factor: int, *, voiced_aware: bool = False) -> np.ndarray:
    """Block-reduce a track by an integer factor.

    ``voiced_aware`` is for pitch: take the median of the *voiced* samples in
    each block, or mark the block unvoiced when fewer than half of them are.
    A plain mean would average real hertz against unvoiced zeros and drag every
    contour toward the floor.
    """
    arr = np.asarray(values, dtype=np.float64)
    if factor <= 1 or arr.size == 0:
        return arr.copy()

    n_blocks = int(np.ceil(arr.size / factor))
    padded = np.full(n_blocks * factor, np.nan)
    padded[: arr.size] = arr
    blocks = padded.reshape(n_blocks, factor)

    if not voiced_aware:
        return np.nanmean(blocks, axis=1)

    out = np.zeros(n_blocks)
    for i, block in enumerate(blocks):
        real = block[~np.isnan(block)]
        if real.size == 0:
            continue
        voiced = real[real > 0]
        if voiced.size * 2 >= real.size and voiced.size > 0:
            out[i] = float(np.median(voiced))
    return out


def auto_pitch_range(
    voiced_hz: np.ndarray,
    *,
    fallback: tuple[float, float] = (60.0, 600.0),
    min_voiced_frames: int = 50,
) -> tuple[float, float]:
    """Narrow the pitch search range to the speakers actually present.

    The two-pass method: measure once with a wide range, then set the floor to
    0.75x the 10th percentile and the ceiling to 1.5x the 90th. A fixed range
    either clips low male creak or high child/female voices; this adapts.

    Falls back to the wide range when there is too little voicing to estimate
    from — percentiles of a nearly-empty array are noise, and a NaN range
    silently produces an invisible pitch line with no error anywhere.
    """
    voiced = np.asarray(voiced_hz, dtype=np.float64)
    voiced = voiced[voiced > 0]
    if voiced.size < min_voiced_frames:
        return fallback

    floor = float(np.percentile(voiced, 10) * 0.75)
    ceiling = float(np.percentile(voiced, 90) * 1.5)
    floor = float(np.clip(floor, 50.0, 300.0))
    ceiling = float(np.clip(ceiling, floor + 50.0, 800.0))
    return (floor, ceiling)
