"""The example figure of docs/math/voice_acoustics.rst.

A synthetic utterance (no participant's voice in the docs), analysed with the
same Praat calls and pitch clean-up as the Acoustics pass (analysis/voice/
praat.py and spectro.py, the pitch range narrowed as the app does), and drawn
in the app's colours (0-5 kHz, magma, pitch cyan, intensity amber), at full
resolution and with pitch and intensity in their own panels.

Run from the repository root with the analysis environment:

    analysis/.venv/Scripts/python.exe docs/figures/voice_acoustics_example.py
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "analysis"))

import matplotlib  # noqa: E402

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker  # noqa: E402
import parselmouth  # noqa: E402
from scipy.signal import lfilter  # noqa: E402
from voice.praat import compute_intensity, compute_pitch, compute_spectrogram  # noqa: E402
from voice.spectro import (  # noqa: E402
    auto_pitch_range,
    drop_short_voiced_runs,
    dynamic_range,
    suppress_low_intensity_pitch,
)

SR = 16_000
OUT = ROOT / "docs" / "_static" / "voice_acoustics_example.png"


def vowel(seconds, f0_from, f0_to, formants, rng):
    """A glottal pulse train with a gliding pitch, through formant resonators."""
    n = int(seconds * SR)
    f0 = np.linspace(f0_from, f0_to, n)
    phase = np.cumsum(f0 / SR)
    src = np.zeros(n)
    src[np.flatnonzero(np.diff(np.floor(phase)) > 0)] = 1.0
    src = lfilter([1.0], [1.0, -0.97], src)  # the glottal pulse's falling spectrum
    out = src
    for f, bw in formants:
        r = np.exp(-np.pi * bw / SR)
        out = lfilter([1 - r], [1, -2 * r * np.cos(2 * np.pi * f / SR), r * r], out)
    env = np.minimum(1.0, np.minimum(np.arange(n), n - np.arange(n)) / (0.03 * SR))
    return out / np.max(np.abs(out)) * env * 0.5 + rng.normal(0, 0.002, n)


def fricative(seconds, rng):
    """'s': noise above about 4 kHz."""
    n = int(seconds * SR)
    x = rng.normal(0, 1, n)
    x = np.diff(np.diff(x, prepend=0), prepend=0)  # tilt the noise towards high frequencies
    env = np.minimum(1.0, np.minimum(np.arange(n), n - np.arange(n)) / (0.02 * SR))
    return x / np.max(np.abs(x)) * env * 0.25


def silence(seconds, rng):
    return rng.normal(0, 0.002, int(seconds * SR))


def main() -> None:
    rng = np.random.default_rng(3)
    parts = [
        ("", silence(0.3, rng)),
        ("vowel [a]", vowel(0.45, 120, 150, [(700, 90), (1200, 110), (2600, 160)], rng)),
        ("[s]", fricative(0.22, rng)),
        ("vowel [i]", vowel(0.40, 145, 110, [(300, 60), (2300, 120), (3000, 180)], rng)),
        ("pause", silence(0.35, rng)),
        ("vowel [u]", vowel(0.40, 110, 105, [(320, 60), (870, 90), (2240, 150)], rng)),
        ("", silence(0.25, rng)),
    ]
    spans, t = [], 0.0
    for label, x in parts:
        spans.append((label, t, t + len(x) / SR))
        t += len(x) / SR
    signal = np.concatenate([x for _, x in parts])
    snd = parselmouth.Sound(signal, sampling_frequency=SR)

    spec = compute_spectrogram(snd, max_frequency=SR * 0.49, time_step=0.002)
    floor_db, ceil_db = dynamic_range(spec.db)
    pt, hz = compute_pitch(snd, floor_hz=60, ceiling_hz=600)
    floor_hz, ceiling_hz = auto_pitch_range(hz)  # the app's second pass
    pt, hz = compute_pitch(snd, floor_hz=floor_hz, ceiling_hz=ceiling_hz)
    it, idb = compute_intensity(snd)
    hz = drop_short_voiced_runs(suppress_low_intensity_pitch(hz, np.interp(pt, it, idb)))
    hz = np.where(hz > 0, hz, np.nan)

    fig, (a1, a2, a3) = plt.subplots(
        3, 1, figsize=(8, 6), sharex=True, gridspec_kw={"height_ratios": [3, 1, 1]}
    )
    shown = spec.freqs_hz <= 5000
    a1.imshow(
        spec.db[shown],
        origin="lower",
        aspect="auto",
        cmap="magma",
        vmin=floor_db,
        vmax=ceil_db,
        extent=[spec.times_s[0], spec.times_s[-1], 0, spec.freqs_hz[shown][-1]],
    )
    a1.set_ylabel("Frequency (Hz)")
    a1.set_title("Spectrogram (0 to 5 kHz, 5 ms window)", loc="left", fontsize=10)
    for label, t0, t1 in spans:
        if label:
            a1.text((t0 + t1) / 2, 4700, label, color="white", ha="center", va="top", fontsize=9)
    # The formants of the first vowel, as dark-to-bright bands.
    for f, name in [(700, "F1"), (1200, "F2"), (2600, "F3")]:
        a1.annotate(
            name,
            xy=(0.32, f),
            xytext=(0.2, f),
            color="white",
            fontsize=8,
            va="center",
            arrowprops={"arrowstyle": "-", "color": "white", "lw": 0.6},
        )

    a2.plot(pt, hz, color="#33e0ff", lw=2)
    a2.set_yscale("log")
    a2.set_ylim(80, 200)
    a2.set_yticks([100, 150])
    a2.set_yticklabels(["100", "150"])
    a2.yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
    a2.set_ylabel("Pitch (Hz)")
    a2.set_title("Pitch: gaps are unvoiced ([s], the pause)", loc="left", fontsize=10)

    a3.plot(it, idb, color="#ffd24a", lw=2)
    a3.set_ylabel("Intensity (dB)")
    a3.set_xlabel("Time (s)")
    a3.set_title("Intensity", loc="left", fontsize=10)
    for ax in (a2, a3):
        ax.grid(True, color="#dddddd", lw=0.5)
        ax.spines[["top", "right"]].set_visible(False)

    fig.tight_layout()
    fig.savefig(OUT, dpi=110)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
