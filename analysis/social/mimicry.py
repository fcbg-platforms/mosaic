"""Mimicry: does one person smile (or nod) soon after the other does, more
often than chance?

For each of A's event onsets, B "follows" when B starts the same kind of event
strictly after it and within the next ``within_s`` seconds (3 s is common for
facial mimicry: Hess and Fischer, 2013). The rate is the share of A's events
that B followed. Two people who simply smile a lot will often follow each other
by coincidence, so the same rate is computed with B's onsets circularly shifted
in time (surrogates), which keeps how often B smiles but breaks the timing.
Mimicry is the real rate minus the surrogates' mean.
"""

from __future__ import annotations

import numpy as np

N_SURROGATES = 200


def _follow_rate(a: np.ndarray, b: np.ndarray, within_s: float) -> float:
    if a.size == 0:
        return np.nan
    b = np.sort(b)
    k = np.searchsorted(b, a, side="right")  # first B onset after each A onset
    nxt = np.where(k < b.size, b[np.minimum(k, b.size - 1)], np.inf)
    return float(np.mean(nxt - a <= within_s))


def mimicry(a_onsets_s, b_onsets_s, span_s: tuple, within_s: float = 3.0, seed: int = 0) -> dict:
    """How often B follows A within ``within_s`` seconds, against chance.

    Parameters
    ----------
    a_onsets_s, b_onsets_s : array_like
        Event onsets, seconds, on one clock.
    span_s : (float, float)
        The stretch both people were measured over; A's events closer than
        ``within_s`` to its end are not counted (B had no chance to follow),
        and surrogate shifts wrap within it.
    """
    lo, hi = span_s
    a = np.asarray([t for t in a_onsets_s if lo <= t <= hi - within_s], float)
    b = np.asarray([t for t in b_onsets_s if lo <= t <= hi], float)
    out = {"events": int(a.size), "partner_events": int(b.size), "within_s": within_s}
    # Shifts of at least 2 x within_s each way need a span over 4 x within_s.
    if a.size == 0 or b.size == 0 or hi - lo <= 4 * within_s:
        return {**out, "followed": 0, "rate": None, "chance_rate": None, "p_value": None}
    rate = _follow_rate(a, b, within_s)
    rng = np.random.default_rng(seed)
    length = hi - lo
    sur = np.array(
        [
            _follow_rate(a, lo + np.mod(b - lo + shift, length), within_s)
            for shift in rng.uniform(2 * within_s, length - 2 * within_s, N_SURROGATES)
        ]
    )
    return {
        **out,
        "followed": int(round(rate * a.size)),
        "rate": round(rate, 4),
        "chance_rate": round(float(sur.mean()), 4),
        "mimicry": round(rate - float(sur.mean()), 4),
        "p_value": round(float((1 + (sur >= rate).sum()) / (1 + sur.size)), 4),
    }
