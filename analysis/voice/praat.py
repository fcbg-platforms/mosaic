"""The parselmouth-touching half of the acoustic pass.

Everything that imports Praat lives here; the numeric decisions live in
:mod:`voice.spectro`, which imports nothing but numpy and is therefore testable
without this package installed. Same split as ``analysis/rppg/``.

Parameter choices below are Praat's own defaults except where a comment says
otherwise, and each exception exists because the default misbehaves on
real room recordings rather than because a different number looked nicer.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import parselmouth
from voice.spectro import WINDOW_LENGTH_S

# Speech energy above ~8 kHz carries almost nothing a reader of a spectrogram
# uses, and halving the sample rate roughly halves the analysis cost.
ANALYSIS_SAMPLE_RATE = 16_000

# 30 s per chunk. The whole file cannot be analysed at once: at Praat's 2 ms
# step a ten-minute recording is a single 300000x401 float64 array — 0.96 GB,
# measured — which is a hard failure, not a slowdown. A chunk is 48 MB.
CHUNK_SECONDS = 30.0


@dataclass
class SpectrogramData:
    """dB values plus the axes they are sampled on."""

    db: np.ndarray  # (n_freq, n_time)
    times_s: np.ndarray
    freqs_hz: np.ndarray


def load_sound(wav_path: Path, *, analysis_sr: int = ANALYSIS_SAMPLE_RATE) -> parselmouth.Sound:
    """Reads a WAV as mono at the analysis rate.

    Mono first, always: a stereo interface produces a two-channel file, and
    Praat's pitch and intensity routines either raise or silently analyse
    channel 1 depending on the call — neither is what the caller wants, and the
    silent one is worse.
    """
    snd = parselmouth.Sound(str(wav_path))
    if snd.n_channels > 1:
        snd = snd.convert_to_mono()
    if snd.sampling_frequency > analysis_sr:
        snd = snd.resample(analysis_sr, 50)
    return snd


def effective_max_frequency(snd: parselmouth.Sound, requested_hz: float) -> float:
    """Clamp the ceiling below Nyquist.

    Asking for exactly Nyquist is legal and yields a top row of nothing but
    reconstruction noise, which renders as a bright band along the top edge that
    looks like signal.
    """
    return float(min(requested_hz, snd.sampling_frequency * 0.49))


def compute_spectrogram(
    snd: parselmouth.Sound,
    *,
    max_frequency: float,
    time_step: float,
    window_length: float = WINDOW_LENGTH_S,
    frequency_step: float = 20.0,
) -> SpectrogramData:
    """One chunk's spectrogram, converted from power to dB.

    ``Spectrogram.values`` is **power** (Pa^2/Hz), not decibels, and is shaped
    ``(n_freq, n_time)`` — verified against parselmouth 0.4.7. Both are easy to
    get backwards and neither mistake announces itself: a transposed array still
    renders as a plausible spectrogram, and treating power as dB produces an
    image that is uniformly wrong rather than obviously broken. Hence the
    assertion.
    """
    sg = snd.to_spectrogram(
        window_length=window_length,
        maximum_frequency=max_frequency,
        time_step=time_step,
        frequency_step=frequency_step,
        window_shape=parselmouth.SpectralAnalysisWindowShape.GAUSSIAN,
    )
    power = np.asarray(sg.values)
    freqs = np.asarray(sg.ys())
    times = np.asarray(sg.xs())
    assert power.shape == (
        len(freqs),
        len(times),
    ), f"expected (n_freq, n_time) = {(len(freqs), len(times))}, got {power.shape}"
    # 1e-14 floors the log at about -140 dB, well below anything audible, and
    # keeps digital silence from becoming -inf and poisoning the percentiles.
    db = 10.0 * np.log10(np.maximum(power, 1e-14))
    return SpectrogramData(db=db, times_s=times, freqs_hz=freqs)


def iter_chunks(
    snd: parselmouth.Sound,
    *,
    chunk_seconds: float = CHUNK_SECONDS,
    window_length: float = WINDOW_LENGTH_S,
):
    """Yields ``(chunk_sound, keep_from_s, keep_to_s)`` covering the whole clip.

    Each chunk is extended by four analysis windows on both sides and the
    padding trimmed afterwards, because the window cannot span a chunk boundary
    — without it there is a visible vertical seam every 30 seconds.

    ``preserve_times=True`` is not optional: without it every chunk's timeline
    restarts at zero and the entire recording collapses into its first chunk.
    """
    pad = 4.0 * window_length
    t = snd.xmin
    while t < snd.xmax:
        keep_to = min(t + chunk_seconds, snd.xmax)
        part = snd.extract_part(
            from_time=max(snd.xmin, t - pad),
            to_time=min(snd.xmax, keep_to + pad),
            preserve_times=True,
        )
        yield part, t, keep_to
        t = keep_to


def compute_pitch(
    snd: parselmouth.Sound,
    *,
    time_step: float = 0.01,
    floor_hz: float = 60.0,
    ceiling_hz: float = 600.0,
    voicing_threshold: float = 0.50,
) -> tuple[np.ndarray, np.ndarray]:
    """(times_s, hz) with 0 Hz meaning unvoiced.

    ``scale_intensity(70)`` first, and this matters more than it looks: Praat's
    ``silence_threshold`` is relative to the file's *global peak*, so a single
    clipped sample makes every later frame register as silent and the pitch
    track comes back empty with no error anywhere.

    ``voicing_threshold`` is raised from Praat's 0.45 because room tone and HVAC
    hum are periodic enough to track as voiced at the default, and those
    readings are indistinguishable from speech on the plot.
    """
    scaled = snd.copy()
    scaled.scale_intensity(70.0)
    pitch = scaled.to_pitch_ac(
        time_step=time_step,
        pitch_floor=floor_hz,
        pitch_ceiling=ceiling_hz,
        very_accurate=False,
        silence_threshold=0.03,
        voicing_threshold=voicing_threshold,
        octave_cost=0.01,
        octave_jump_cost=0.35,
        voiced_unvoiced_cost=0.14,
    )
    return np.asarray(pitch.xs()), np.asarray(pitch.selected_array["frequency"])


def compute_intensity(
    snd: parselmouth.Sound, *, time_step: float = 0.01, minimum_pitch: float = 100.0
) -> tuple[np.ndarray, np.ndarray]:
    """(times_s, dB). ``minimum_pitch`` sets the window: 3.2/100 = 32 ms, Praat's default."""
    intensity = snd.to_intensity(
        minimum_pitch=minimum_pitch, time_step=time_step, subtract_mean=True
    )
    # .values is (1, n) — a single channel in a 2-D container.
    return np.asarray(intensity.xs()), np.asarray(intensity.values)[0]
