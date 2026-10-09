"""
Eye contact and gaze aversion from per-frame gaze angles.

Pure numpy, so every rule is unit-tested.

**Where the partner is.** In an interview the camera sees the interviewee;
the interviewer sits somewhere off camera. Listeners look at whoever is
speaking much of the time (Kendon, 1967; Argyle and Cook, 1976), so while the
subject listens their gaze clusters on the interviewer. That cluster's
centre, the **mode** of the gaze angles (:func:`find_mode`), is taken as the
interviewer's direction. Because it comes from the same measurements, a
constant bias of the gaze estimate (kappa differs between people by a few
degrees; an uncalibrated lens tilts all angles a little) moves the cluster
and the gaze together and cancels. The partner can instead be the camera
itself (a remote interview, or the interviewer right beside the camera) or a
direction given by hand.

**Eye contact** is gaze within :func:`contact_radius` of that direction: 2.5
times the cluster's own spread, between :data:`MIN_RADIUS_DEG` and
:data:`MAX_RADIUS_DEG`. Measurement noise and the size of a face at
conversation distance (about 7 to 10 degrees across) set that spread. True
mutual eye contact cannot be told apart from looking at the face; this is
"looking at the partner's face", the usual meaning in interview research.

**Gaze aversion** is looking away for at least :data:`MIN_AVERSION_S`;
shorter excursions are saccades and noise. Each aversion gets a direction
(up, down, or towards the subject's left or right). Looking up and away while
answering goes with thinking (Glenberg, Schroeder and Robertson, 1998;
Doherty-Sneddon and Phelps, 2005); speakers look away at the start of a turn
and back at its end (Kendon, 1967; Ho, Foulsham and Kingstone, 2015).
"""

from __future__ import annotations

import math

import numpy as np

#: Histogram range and resolution for the mode, degrees.
MODE_RANGE_DEG = 60.0
MODE_BIN_DEG = 1.0
MODE_SMOOTH_DEG = 2.0
#: Mean-shift refinement radius around the histogram peak, degrees.
MODE_REFINE_DEG = 5.0
#: Points this close to the partner's direction measure the cluster's spread.
CORE_DEG = 10.0
#: The contact cone: this many times the spread, within these limits.
RADIUS_SIGMAS = 2.5
MIN_RADIUS_DEG, MAX_RADIUS_DEG = 5.0, 15.0
#: Look-aways shorter than this are not aversions, seconds.
MIN_AVERSION_S = 0.3
#: Brief returns inside a look-away shorter than this do not end it, seconds.
MIN_RETURN_S = 0.15
#: Gaps in the gaze (blinks, a lost face) up to this long take the state
#: around them when it is the same on both sides, seconds.
MAX_FILL_S = 0.5
#: Turn analysis: the start window, the end window, and the shortest turn.
TURN_START_S, TURN_END_S, MIN_TURN_S = 2.0, 1.0, 2.0


def to_direction(yaw_deg, pitch_deg) -> np.ndarray:
    """Unit directions ``(N, 3)`` from camera-frame gaze angles (see
    :func:`eye_contact.gaze.gaze_angles`)."""
    y = np.radians(np.asarray(yaw_deg, dtype=np.float64))
    p = np.radians(np.asarray(pitch_deg, dtype=np.float64))
    return np.stack([np.cos(p) * np.sin(y), -np.sin(p), -np.cos(p) * np.cos(y)], axis=-1)


def angle_between(a, b) -> np.ndarray:
    """Angle in degrees between direction arrays (``nan`` stays ``nan``)."""
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    cross = np.linalg.norm(np.cross(a, b), axis=-1)
    dot = np.sum(a * b, axis=-1)
    return np.degrees(np.arctan2(cross, dot))


def find_mode(yaw_deg, pitch_deg) -> tuple[float, float] | None:
    """The densest gaze direction: the peak of a smoothed 2D histogram,
    refined by mean shift. ``None`` without data."""
    y = np.asarray(yaw_deg, dtype=np.float64)
    p = np.asarray(pitch_deg, dtype=np.float64)
    ok = (
        np.isfinite(y)
        & np.isfinite(p)
        & (np.abs(y) < MODE_RANGE_DEG)
        & (np.abs(p) < MODE_RANGE_DEG)
    )
    if ok.sum() < 10:
        return None
    y, p = y[ok], p[ok]
    edges = np.arange(-MODE_RANGE_DEG, MODE_RANGE_DEG + MODE_BIN_DEG, MODE_BIN_DEG)
    h, _, _ = np.histogram2d(y, p, bins=[edges, edges])
    s = MODE_SMOOTH_DEG / MODE_BIN_DEG
    k = np.exp(-0.5 * (np.arange(-3 * s, 3 * s + 1) / s) ** 2)
    h = np.apply_along_axis(lambda r: np.convolve(r, k, mode="same"), 0, h)
    h = np.apply_along_axis(lambda r: np.convolve(r, k, mode="same"), 1, h)
    i, j = np.unravel_index(int(np.argmax(h)), h.shape)
    cy, cp = edges[i] + MODE_BIN_DEG / 2, edges[j] + MODE_BIN_DEG / 2
    for _ in range(10):
        near = np.hypot(y - cy, p - cp) <= MODE_REFINE_DEG
        if not near.any():
            break
        ny, npitch = float(np.mean(y[near])), float(np.mean(p[near]))
        moved = math.hypot(ny - cy, npitch - cp)
        cy, cp = ny, npitch
        if moved < 0.01:
            break
    return float(cy), float(cp)


def contact_radius(offsets_deg) -> tuple[float, float, float]:
    """``(radius, sigma, share)``: the contact cone from the spread of the
    gaze around the partner's direction. ``sigma`` is a robust per-axis
    standard deviation of the points within :data:`CORE_DEG` (from the median
    distance: for a 2D Gaussian it is 1.177 sigma), and ``share`` the share of
    all points inside the cone."""
    o = np.asarray(offsets_deg, dtype=np.float64)
    o = o[np.isfinite(o)]
    if o.size == 0:
        return MIN_RADIUS_DEG, float("nan"), 0.0
    core = o[o <= CORE_DEG]
    sigma = float(np.median(core) / 1.177) if core.size else float("nan")
    radius = MIN_RADIUS_DEG if not math.isfinite(sigma) else RADIUS_SIGMAS * sigma
    radius = float(min(MAX_RADIUS_DEG, max(MIN_RADIUS_DEG, radius)))
    return radius, sigma, float(np.mean(o <= radius))


def _runs(values: np.ndarray, value) -> list[tuple[int, int]]:
    m = values == value
    edges = np.flatnonzero(np.diff(np.concatenate([[False], m, [False]]).astype(np.int8)))
    return list(zip(edges[::2].tolist(), edges[1::2].tolist(), strict=True))


def _step(times: np.ndarray) -> float:
    """The typical frame interval (computed once per call: it is a median
    over the whole recording)."""
    return float(np.median(np.diff(times))) if len(times) > 1 else 0.0


def _span(times: np.ndarray, i0: int, i1: int, step: float) -> float:
    """Duration of frames ``[i0, i1)``: their span plus one frame."""
    return float(times[i1 - 1] - times[i0]) + step


def contact_states(offsets_deg, radius: float, times_s) -> np.ndarray:
    """Per frame 1 (eye contact), 0 (looking away) or ``nan`` (unknown).

    In order: brief returns inside a look-away are ignored; look-aways
    shorter than :data:`MIN_AVERSION_S` next to contact count as contact;
    then short gaps (blinks) between equal states take that state. Gaps are
    filled last because the iris jumps as the lid closes, which leaves a
    flicker of "away" beside many blinks.
    """
    t = np.asarray(times_s, dtype=np.float64)
    o = np.asarray(offsets_deg, dtype=np.float64)
    step = _step(t)
    state = np.where(np.isfinite(o), (o <= radius).astype(np.float64), np.nan)
    code = np.where(np.isnan(state), -1, state).astype(np.int8)
    n = len(code)

    def fill_gaps():
        for i0, i1 in _runs(code, -1):
            if (
                0 < i0
                and i1 < n
                and code[i0 - 1] == code[i1]
                and _span(t, i0, i1, step) <= MAX_FILL_S
            ):
                code[i0:i1] = code[i0 - 1]

    fill_gaps()
    for i0, i1 in _runs(code, 1):
        if (
            0 < i0
            and i1 < n
            and code[i0 - 1] == 0
            and code[i1] == 0
            and _span(t, i0, i1, step) < MIN_RETURN_S
        ):
            code[i0:i1] = 0
    for i0, i1 in _runs(code, 0):
        beside_contact = (i0 > 0 and code[i0 - 1] == 1) or (i1 < n and code[i1] == 1)
        if beside_contact and _span(t, i0, i1, step) < MIN_AVERSION_S:
            code[i0:i1] = 1
    fill_gaps()
    return np.where(code < 0, np.nan, code.astype(np.float64))


def direction_label(d_yaw: float, d_pitch: float) -> str:
    """``up``, ``down``, ``left`` or ``right`` (the subject's own left and
    right: camera yaw grows towards image right, the subject's left)."""
    if abs(d_pitch) >= abs(d_yaw):
        return "up" if d_pitch > 0 else "down"
    return "left" if d_yaw > 0 else "right"


def aversions(state, times_s, yaw_deg, pitch_deg, target_yaw, target_pitch) -> list[dict]:
    """Every look-away: start, end, duration, mean offset and direction.
    ``target_yaw``/``target_pitch`` are per-frame arrays or numbers."""
    t = np.asarray(times_s, dtype=np.float64)
    step = _step(t)
    code = np.where(np.isnan(state), -1, state).astype(np.int8)
    dy = np.asarray(yaw_deg, float) - np.broadcast_to(np.asarray(target_yaw, float), t.shape)
    dp = np.asarray(pitch_deg, float) - np.broadcast_to(np.asarray(target_pitch, float), t.shape)
    out = []
    for i0, i1 in _runs(code, 0):
        my, mp = np.nanmean(dy[i0:i1]), np.nanmean(dp[i0:i1])
        if not (np.isfinite(my) and np.isfinite(mp)):
            continue
        out.append(
            {
                "start_s": float(t[i0]),
                "end_s": float(t[i0]) + _span(t, i0, i1, step),
                "duration_s": _span(t, i0, i1, step),
                "offset_deg": float(math.hypot(my, mp)),
                "d_yaw": float(my),
                "d_pitch": float(mp),
                "direction": direction_label(my, mp),
            }
        )
    return out


def mask_from_intervals(times_s, intervals) -> np.ndarray:
    """Frames inside any ``[start, end)`` interval."""
    t = np.asarray(times_s, dtype=np.float64)
    m = np.zeros(len(t), bool)
    for a, b in intervals:
        m[(t >= a) & (t < b)] = True
    return m


def share(state, mask) -> float | None:
    """Percent of known frames in ``mask`` with eye contact."""
    s = np.asarray(state, dtype=np.float64)
    sel = np.asarray(mask, bool) & np.isfinite(s)
    return round(100.0 * float(np.mean(s[sel])), 1) if sel.sum() >= 10 else None


def _overlaps(av: list[dict], a: float, b: float) -> bool:
    return any(x["start_s"] < b and x["end_s"] > a for x in av)


def turn_patterns(state, times_s, av: list[dict], subject_turns, response_gaps) -> dict:
    """How gaze lines up with the subject's turns.

    * ``turn_start_aversion_pct``: subject turns of at least
      :data:`MIN_TURN_S` with an aversion in their first :data:`TURN_START_S`.
    * ``turn_end_contact_pct``: those turns with eye contact for most of their
      last :data:`TURN_END_S` (looking back to hand over the turn), over the
      ``turn_ends_measured`` turns whose last second has a gaze.
    * ``gap_aversion_pct``: response gaps (the silence before the subject
      answers) with an aversion.
    """
    t = np.asarray(times_s, dtype=np.float64)
    s = np.asarray(state, dtype=np.float64)
    long_turns = [(a, b) for a, b in subject_turns if b - a >= MIN_TURN_S]
    starts = [_overlaps(av, a, a + TURN_START_S) for a, _ in long_turns]
    ends = []
    for _, b in long_turns:
        sel = (t >= b - TURN_END_S) & (t < b) & np.isfinite(s)
        if sel.sum() >= 3:
            ends.append(float(np.mean(s[sel])) >= 0.5)
    gaps = [_overlaps(av, a, b) for a, b in response_gaps if b > a]

    def pct(xs):
        return round(100.0 * float(np.mean(xs)), 1) if xs else None

    return {
        "subject_turns": len(long_turns),
        "turn_start_aversion_pct": pct(starts),
        "turn_end_contact_pct": pct(ends),
        "turn_ends_measured": len(ends),
        "response_gaps": len(gaps),
        "gap_aversion_pct": pct(gaps),
    }


def summarise(state, times_s, av: list[dict], speaking=None, listening=None, covered=None) -> dict:
    """Overall numbers; with speaking/listening masks also per state.
    ``covered`` limits "silence" to the frames whose speech was analysed."""
    t = np.asarray(times_s, dtype=np.float64)
    s = np.asarray(state, dtype=np.float64)
    known = np.isfinite(s)
    step = _step(t)
    known_s = float(known.sum() * step)
    minutes = known_s / 60.0
    durations = [x["duration_s"] for x in av]
    by_dir = {d: sum(1 for x in av if x["direction"] == d) for d in ("up", "down", "left", "right")}
    out = {
        "measured_s": round(known_s, 1),
        "measured_pct": round(100.0 * float(known.mean()), 1) if len(s) else 0.0,
        "eye_contact_pct": share(s, np.ones(len(s), bool)),
        "aversions": len(av),
        "aversions_per_min": round(len(av) / minutes, 2) if minutes > 0 else None,
        "aversion_median_s": round(float(np.median(durations)), 2) if durations else None,
        "aversion_directions": by_dir,
        "longest_contact_s": None,
    }
    contact_runs = [
        _span(t, a, b, step) for a, b in _runs(np.where(np.isnan(s), -1, s).astype(np.int8), 1)
    ]
    if contact_runs:
        out["longest_contact_s"] = round(max(contact_runs), 2)
        out["contact_median_s"] = round(float(np.median(contact_runs)), 2)
    if speaking is not None and listening is not None:
        out["eye_contact_speaking_pct"] = share(s, speaking)
        out["eye_contact_listening_pct"] = share(s, listening)
        cov = np.ones(len(s), bool) if covered is None else np.asarray(covered, bool)
        quiet = cov & ~np.asarray(speaking) & ~np.asarray(listening)
        out["eye_contact_silence_pct"] = share(s, quiet)
    return out
