"""
Speech activity from a recording's audio, on a 10 ms grid.

Speech is found by **Silero VAD** (:func:`detect_speech_silero`), a small
neural voice-activity detector that ships with faster-whisper (already
installed for Speaker Diarization), run on 16 kHz audio with 32 ms
resolution. Unlike an energy threshold it does not mistake fans, machines or
music for speech.

The speech-band energy (150 to 4000 Hz, :func:`band_energy_db`) is still
computed: it is the audio level drawn in the app, and what lip movement is
lined up against (:func:`conversation.timing.estimate_av_lag`).
:func:`detect_speech` (an adaptive energy threshold) remains as the fallback
when faster-whisper is not installed.

This finds *that* someone is speaking, not who: :mod:`conversation.speakers`
decides that.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np

#: Analysis grid, seconds.
HOP_S = 0.01
WINDOW_S = 0.025
BAND_HZ = (150.0, 4000.0)
#: Where the thresholds sit between the noise floor and the speech level.
ON_FRACTION, OFF_FRACTION = 0.35, 0.25
#: At least this many dB above the noise floor, however quiet the speech.
MIN_ON_DB = 8.0
#: Band energy below this is digital silence (zero samples), dB.
DIGITAL_SILENCE_DB = -100.0
#: Silences shorter than this inside speech are filled, seconds.
FILL_GAP_S = 0.15
#: Speech bursts shorter than this are dropped, seconds.
MIN_SPEECH_S = 0.1


def read_wav(path: str | Path) -> tuple[np.ndarray, int]:
    """Mono float samples in [-1, 1] and the sample rate."""
    from scipy.io import wavfile

    rate, data = wavfile.read(str(path), mmap=True)
    x = np.asarray(data)
    # Mix to mono before converting, in float32: an hour of 44.1 kHz stereo
    # is 0.6 GB as int16 but would be 2.5 GB as float64 stereo.
    if x.ndim == 2:
        x = x.mean(axis=1, dtype=np.float32)
    if x.dtype.kind == "i":
        scale = float(np.iinfo(data.dtype).max)
        x = x.astype(np.float32) / np.float32(scale)
    elif x.dtype.kind == "u":
        x = (x.astype(np.float32) - 128.0) / 128.0
    else:
        x = x.astype(np.float32)
    return np.ascontiguousarray(x), int(rate)


def band_energy_db(x: np.ndarray, rate: int, hop_s: float = HOP_S) -> np.ndarray:
    """Speech-band energy in dB per hop; frame ``i`` is centred on sample
    ``i * hop``."""
    hop = int(round(hop_s * rate))
    win = int(round(WINDOW_S * rate))
    n_fft = 1 << (win - 1).bit_length()
    n = len(x) // hop
    if n == 0:
        return np.zeros(0)
    pad = np.concatenate([np.zeros(win // 2), x, np.zeros(win)])
    freqs = np.fft.rfftfreq(n_fft, 1.0 / rate)
    band = (freqs >= BAND_HZ[0]) & (freqs <= BAND_HZ[1])
    window = np.hanning(win)
    out = np.empty(n)
    chunk = 4096
    for c0 in range(0, n, chunk):
        idx = np.arange(c0, min(n, c0 + chunk))
        frames = pad[idx[:, None] * hop + np.arange(win)[None, :]] * window
        spec = np.abs(np.fft.rfft(frames, n_fft, axis=1)) ** 2
        out[idx] = 10.0 * np.log10(spec[:, band].sum(axis=1) + 1e-12)
    return out


def _fill_and_drop(mask: np.ndarray, hop_s: float) -> np.ndarray:
    """Fill silences shorter than :data:`FILL_GAP_S`, then drop speech
    shorter than :data:`MIN_SPEECH_S`."""
    m = mask.copy()
    for value, max_len in ((False, FILL_GAP_S), (True, MIN_SPEECH_S)):
        for i0, i1 in runs(m, value):
            inside = i0 > 0 and i1 < len(m)  # a run at the edge is not a gap
            if (i1 - i0) * hop_s < max_len and (inside or value):
                m[i0:i1] = not value
    return m


#: Silero settings: speech probability threshold, and the shortest speech and
#: silence it reports, milliseconds. A little padding keeps word edges.
SILERO_THRESHOLD = 0.5
SILERO_MIN_SPEECH_MS = 100
SILERO_MIN_SILENCE_MS = 100
SILERO_PAD_MS = 30


def detect_speech_silero(
    x: np.ndarray, rate: int, n_hops: int, hop_s: float = HOP_S
) -> np.ndarray | None:
    """Speech mask on the hop grid from Silero VAD, or ``None`` when
    faster-whisper is not installed."""
    try:
        from faster_whisper.vad import VadOptions, get_speech_timestamps
    except ImportError:
        return None
    from math import gcd

    from scipy.signal import resample_poly

    g = gcd(16000, rate)
    y = resample_poly(np.asarray(x, dtype=np.float64), 16000 // g, rate // g).astype(np.float32)
    options = VadOptions(
        threshold=SILERO_THRESHOLD,
        min_speech_duration_ms=SILERO_MIN_SPEECH_MS,
        min_silence_duration_ms=SILERO_MIN_SILENCE_MS,
        speech_pad_ms=SILERO_PAD_MS,
    )
    mask = np.zeros(n_hops, bool)
    centres = np.arange(n_hops) * hop_s
    for seg in get_speech_timestamps(y, options):
        mask[(centres >= seg["start"] / 16000.0) & (centres < seg["end"] / 16000.0)] = True
    return mask


def runs(mask: np.ndarray, value: bool = True) -> list[tuple[int, int]]:
    """Half-open index runs ``[i0, i1)`` where ``mask == value``."""
    m = np.asarray(mask, dtype=bool) == value
    edges = np.flatnonzero(np.diff(np.concatenate([[False], m, [False]]).astype(np.int8)))
    return list(zip(edges[::2].tolist(), edges[1::2].tolist(), strict=True))


def detect_speech(db: np.ndarray, hop_s: float = HOP_S) -> tuple[np.ndarray, dict]:
    """Speech mask from band energy, and the levels used.

    Returns
    -------
    tuple
        ``(mask, levels)``: a boolean mask per hop and ``{"noise_db",
        "speech_db", "on_db", "off_db"}``.
    """
    db = np.asarray(db, dtype=np.float64)
    # Digital silence (exact zeros: before the first buffer, or a dropout)
    # is not the room's noise floor and would drag the thresholds down.
    live = db > DIGITAL_SILENCE_DB
    if live.sum() < 10:
        return np.zeros(len(db), bool), {}
    noise = float(np.percentile(db[live], 10))
    speech = float(np.percentile(db[live], 95))
    span = max(speech - noise, 0.0)
    on = noise + max(MIN_ON_DB, ON_FRACTION * span)
    off = noise + max(MIN_ON_DB * OFF_FRACTION / ON_FRACTION, OFF_FRACTION * span)
    mask = np.zeros(len(db), bool)
    active = False
    for i, v in enumerate(db):
        active = (v >= off if active else v >= on) and live[i]
        mask[i] = active
    levels = {"noise_db": noise, "speech_db": speech, "on_db": on, "off_db": off}
    return _fill_and_drop(mask, hop_s), levels
