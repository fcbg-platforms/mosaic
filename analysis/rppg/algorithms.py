"""
Pure signal-combination algorithms for remote heart-rate estimation (rPPG).

No mediapipe/cv2/scipy import here — these three functions take only a
plain ``(N, 3)`` array of per-frame ROI-mean RGB values (already extracted
by :mod:`rppg.roi`) and return a single combined 1-D pulse signal. Kept
completely free of I/O/model dependencies specifically so this — the one
piece of math whose correctness genuinely matters, since a wrong sign or
transposed coefficient would silently produce a plausible-looking but wrong
heart rate — is directly unit-testable, matching this project's established
"isolate the pure math, test it directly" discipline (e.g. diarize/
pipeline.py's assign_speakers(), transcribe/windowing.py's
confirm_segments()).

Every formula below was independently verified against a primary source
(the original paper and/or a real, cited reference implementation) before
being written down, not reconstructed from memory — see each function's own
docstring for its exact source and any ambiguity found during that
verification.
"""
from __future__ import annotations

from typing import Callable

import numpy as np


def normalize_channels(rgb: np.ndarray) -> np.ndarray:
    """Divide each channel by its own temporal mean.

    ``Cn = C / mean(C)`` — the standard first step shared by CHROM and
    POS: it removes each channel's own DC/brightness level so that
    illumination differences between R, G, and B don't dominate over the
    much smaller pulse-induced color variation the whole method exists to
    recover.

    Parameters
    ----------
    rgb : numpy.ndarray
        Shape ``(N, 3)``, columns R, G, B, rows are per-frame temporal
        samples within one analysis window.

    Returns
    -------
    numpy.ndarray
        Same shape, each column divided by its own mean.

    Raises
    ------
    ValueError
        If any channel's temporal mean is exactly zero (a degenerate
        all-black window) — dividing would produce ``inf``/``nan``.
    """
    means = rgb.mean(axis=0)
    if np.any(means == 0):
        raise ValueError("cannot normalize a channel with zero temporal mean")
    return rgb / means


def green_signal(rgb: np.ndarray) -> np.ndarray:
    """Naive baseline: the raw green channel, mean-centered.

    Verkruysse, Svaasand & Nelson (2008), "Remote plethysmographic
    imaging using ambient light" — the original, simplest rPPG method.
    Green has the strongest hemoglobin-absorption response of the three
    channels, but this method has no motion/illumination compensation at
    all — kept as a fast baseline for comparison/debugging, not the
    default backend.

    Parameters
    ----------
    rgb : numpy.ndarray
        Shape ``(N, 3)``, columns R, G, B.

    Returns
    -------
    numpy.ndarray
        Shape ``(N,)``, the mean-centered green channel.
    """
    g = rgb[:, 1]
    return g - g.mean()


def chrom_signal(rgb: np.ndarray) -> np.ndarray:
    """CHROM — chrominance-based pulse extraction.

    de Haan & Jeanne, IEEE TBME 2013, "Robust Pulse Rate From
    Chrominance-Based rPPG" — chrominance signals
    ``Ω = 3R − 2G``, ``Φ = 1.5R + G − 1.5B``, alpha-tuned combination.

    Applies the standard temporal-mean normalization (via
    :func:`normalize_channels`) before the chrominance projection — the
    theoretically-required step for CHROM's motion/illumination
    cancellation to hold: without it, the DC brightness term dominates
    and the paper's own skin-reflection-model argument for why the
    chrominance signals cancel illumination changes no longer applies.

    Notes
    -----
    One real ambiguity, surfaced honestly rather than silently resolved
    during verification: a reference implementation inspected while
    building this (``phuselab/pyVHR``, ``cpu_CHROM``) applies the
    ``3R−2G`` / ``1.5R+G−1.5B`` formula directly to **raw**
    (non-normalized) RGB in the one function body actually retrieved —
    normalization may happen upstream in that library's own
    RGB-extraction stage, which wasn't independently confirmed. If
    CHROM's output quality looks wrong in practice, re-verify this
    normalization choice against the original IEEE paper's own equations
    directly, not just a secondary implementation, before assuming a bug
    elsewhere.

    Parameters
    ----------
    rgb : numpy.ndarray
        Shape ``(N, 3)``, columns R, G, B.

    Returns
    -------
    numpy.ndarray
        Shape ``(N,)``, the combined chrominance pulse signal. All-zero
        if the window is degenerate (``Φ``'s temporal std is ~0).
    """
    rn, gn, bn = normalize_channels(rgb).T
    xc = 3 * rn - 2 * gn
    yc = 1.5 * rn + gn - 1.5 * bn
    std_yc = np.std(yc)
    if std_yc < 1e-12:
        return np.zeros_like(xc)
    alpha = np.std(xc) / std_yc
    return xc - alpha * yc


def pos_signal(rgb: np.ndarray) -> np.ndarray:
    """POS — Plane-Orthogonal-to-Skin pulse extraction (default backend).

    Wang, den Brinker, Stuijk & de Haan, IEEE TBME 2017, "Algorithmic
    Principles of Remote-PPG" — generally regarded as the best classical
    (non-deep-learning) rPPG method. Verified directly against a real,
    cited reference implementation (``pavisj/rppg-pos``,
    ``pos_face_seg.py``) rather than reconstructed from memory: temporal
    normalization, then the fixed projection matrix ``[[0,1,-1],
    [-2,1,1]]`` applied to ``Cn = [Rn; Gn; Bn]``, i.e.
    ``Xs = Gn − Bn``, ``Ys = −2·Rn + Gn + Bn``, combined as
    ``α = std(Xs)/std(Ys)``, ``S = Xs + α·Ys``.

    Notes
    -----
    Deliberate simplification vs. the reference implementation: the
    reference runs this projection over short (~1.6s) overlapping windows
    with overlap-add reconstruction, tuned for real-time streaming use.
    This implementation applies one projection per (longer,
    caller-supplied) HR-analysis window instead — a documented, understood
    divergence from that streaming-specific implementation detail, not a
    misunderstanding of the underlying algorithm.

    Parameters
    ----------
    rgb : numpy.ndarray
        Shape ``(N, 3)``, columns R, G, B.

    Returns
    -------
    numpy.ndarray
        Shape ``(N,)``, the combined POS pulse signal. All-zero if the
        window is degenerate (``Ys``'s temporal std is ~0).
    """
    rn, gn, bn = normalize_channels(rgb).T
    xs = gn - bn
    ys = -2 * rn + gn + bn
    std_ys = np.std(ys)
    if std_ys < 1e-12:
        return np.zeros_like(xs)
    alpha = np.std(xs) / std_ys
    return xs + alpha * ys


#: Backend-name -> pure signal-combination function, dispatched by
#: run_rppg.py's --backend argument.
BACKENDS: dict[str, Callable[[np.ndarray], np.ndarray]] = {
    "green": green_signal,
    "chrom": chrom_signal,
    "pos": pos_signal,
}
