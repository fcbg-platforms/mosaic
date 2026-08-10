"""16-bit interleaved PCM -> mono float32 16kHz, for feeding faster-whisper
directly as a numpy array (bypassing its internal ffmpeg-based file
decoder). scipy is already a mosaic-analysis dependency (see
analysis/diarize/pipeline.py's own scipy.io.wavfile use)."""
from __future__ import annotations

from math import gcd

import numpy as np
from scipy.signal import resample_poly

WHISPER_SAMPLE_RATE = 16000


def pcm16_to_mono_float32(payload: bytes, channels: int) -> np.ndarray:
    """Interleaved int16 LE PCM -> mono float32 in [-1, 1]."""
    samples = np.frombuffer(payload, dtype="<i2").reshape(-1, channels).astype(np.float32) / 32768.0
    return samples.mean(axis=1) if channels > 1 else samples[:, 0]


def resample_to_16k(mono: np.ndarray, source_rate_hz: int) -> np.ndarray:
    """No-op if already 16kHz (the common case is NOT 16kHz — see
    audio_recorder.cpp's device-negotiation doc comment, e.g. this dev
    machine's real mic negotiates 48000Hz/2ch)."""
    if source_rate_hz == WHISPER_SAMPLE_RATE:
        return mono
    g = gcd(WHISPER_SAMPLE_RATE, source_rate_hz)
    up, down = WHISPER_SAMPLE_RATE // g, source_rate_hz // g
    return resample_poly(mono, up, down).astype(np.float32)
