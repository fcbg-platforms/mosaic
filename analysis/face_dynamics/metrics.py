"""
Face dynamics from per-frame face measurements: blinks, expression events,
expressivity and head gestures.

Pure numpy, no video or model code, so every rule here is unit-tested.
The per-frame inputs come from MediaPipe FaceLandmarker
(:mod:`face_dynamics.extract`): eye openness from the eyelid landmarks, the
52 expression "blendshapes" (each 0..1, close relatives of the Facial Action
Coding System's action units) and the head's yaw, pitch and roll.

Everything is relative to the person's own baseline where it matters: eyes
differ in shape and faces in resting expression, so absolute thresholds would
measure the face rather than what it does.

Interview mode (one camera, a large face, about 50 fps) is what makes this
work: a blink lasts 100 to 300 ms, so 5 to 15 frames at 50 fps, enough to
measure its duration; at the 12 to 25 fps of a room recording a blink is 1 to
7 frames and durations are coarse.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

# ── Blinks ───────────────────────────────────────────────────────────────────

#: Openness (eye opening relative to this person's open-eye baseline) below
#: which a closure starts, and above which it ends. Two thresholds, so noise
#: around one value cannot split one blink into several.
BLINK_CLOSE = 0.5
BLINK_REOPEN = 0.7
#: Blink durations, ms. Shorter is noise; longer is an eye closure, reported
#: separately (it is not a blink, and would skew blink statistics).
BLINK_MIN_MS = 50.0
BLINK_MAX_MS = 500.0
#: PERCLOS counts the time the eyes are at least 80% closed (openness <= 0.2),
#: the standard drowsiness measure (Wierwille et al., 1994).
PERCLOS_OPENNESS = 0.2
#: The open-eye baseline: this percentile of eye opening over a sliding
#: window, so it follows slow changes (posture, lighting, squinting while
#: smiling) but not blinks. Blinks take only 5 to 7% of the time, so the 80th
#: percentile of 6 s still sits on the open eye, even around a 1.5 s closure.
BASELINE_PERCENTILE = 80.0
BASELINE_WINDOW_S = 6.0


def eye_aspect_ratio(p) -> float:
    """Eye aspect ratio (Soukupova and Cech, 2016) from six eye landmarks
    ``p1..p6`` (outer corner, two upper lid points, inner corner, two lower
    lid points): ``(|p2-p6| + |p3-p5|) / (2 |p1-p4|)``. About 0.25 to 0.35 for
    an open eye, near 0 when closed."""
    q = np.asarray(p, dtype=np.float64).reshape(6, -1)
    width = np.linalg.norm(q[0] - q[3])
    if width < 1e-9:
        return float("nan")
    return float((np.linalg.norm(q[1] - q[5]) + np.linalg.norm(q[2] - q[4])) / (2.0 * width))


def rolling_percentile(values, times_s, window_s: float, q: float) -> np.ndarray:
    """``q``-th percentile of the finite values within ``window_s / 2`` of each
    sample (``nan`` where the window holds none)."""
    v = np.asarray(values, dtype=np.float64)
    t = np.asarray(times_s, dtype=np.float64)
    out = np.full(len(v), np.nan)
    half = window_s / 2.0
    lo = hi = 0
    finite = np.isfinite(v)
    for i in range(len(v)):
        while t[lo] < t[i] - half:
            lo += 1
        while hi < len(v) and t[hi] <= t[i] + half:
            hi += 1
        w = v[lo:hi][finite[lo:hi]]
        if w.size:
            out[i] = np.percentile(w, q)
    return out


def openness(ear, times_s, window_s: float = BASELINE_WINDOW_S) -> np.ndarray:
    """Eye opening relative to the open-eye baseline: about 1 open, 0 closed."""
    base = rolling_percentile(ear, times_s, window_s, BASELINE_PERCENTILE)
    with np.errstate(divide="ignore", invalid="ignore"):
        return np.clip(np.asarray(ear, dtype=np.float64) / base, 0.0, 1.5)


@dataclass
class Event:
    """A span of time with one behaviour.

    Attributes
    ----------
    kind : str
        ``"blink"``, ``"long_closure"``, ``"smile"``, ``"duchenne_smile"``,
        ``"brow_raise"``, ``"nod"`` or ``"shake"``.
    start_s, end_s : float
        Start of its first frame and end of its last (the last frame's time
        plus one frame), so ``end_s - start_s`` is its length.
    peak_s : float
        Time of the strongest frame.
    peak : float
        Strength at that frame (meaning depends on the kind: closure depth,
        blendshape score, swing in degrees).
    extra : dict
        Kind-specific details (e.g. a nod's number of swings).
    """

    kind: str
    start_s: float
    end_s: float
    peak_s: float
    peak: float
    extra: dict = field(default_factory=dict)

    @property
    def duration_ms(self) -> float:
        return (self.end_s - self.start_s) * 1000.0


def _step(times: np.ndarray) -> float:
    """The typical frame interval."""
    return float(np.median(np.diff(times))) if len(times) > 1 else 0.0


def _frame_span(times: np.ndarray, i0: int, i1: int) -> float:
    """Duration covered by frames i0..i1: their time span plus one frame."""
    return times[i1] - times[i0] + _step(times)


def _end(times: np.ndarray, i1: int) -> float:
    """End time of frame ``i1``: its time plus one frame."""
    return float(times[i1]) + _step(times)


def hysteresis_spans(signal, on: float, off: float, below: bool = False) -> list[tuple[int, int]]:
    """Index spans ``(first, last)`` where a signal crosses ``on`` and stays
    until it crosses back over ``off``. With ``below`` the signal must drop
    below ``on`` (and rise above ``off`` to end). ``nan`` ends a span."""
    x = np.asarray(signal, dtype=np.float64)
    spans, start = [], None
    for i, v in enumerate(x):
        if not np.isfinite(v):
            if start is not None:
                spans.append((start, i - 1))
                start = None
            continue
        active = (v < on) if below else (v > on)
        release = (v > off) if below else (v < off)
        if start is None and active:
            start = i
        elif start is not None and release:
            spans.append((start, i - 1))
            start = None
    if start is not None:
        spans.append((start, len(x) - 1))
    return spans


def detect_blinks(open_left, open_right, times_s) -> list[Event]:
    """Blinks and long closures from both eyes' openness.

    A closure needs both eyes (their mean drops below :data:`BLINK_CLOSE`):
    one eye alone is a wink or a tracking error. Closures of
    :data:`BLINK_MIN_MS` to :data:`BLINK_MAX_MS` are blinks; longer ones are
    ``long_closure`` events.
    """
    t = np.asarray(times_s, dtype=np.float64)
    both = (
        np.asarray(open_left, dtype=np.float64) + np.asarray(open_right, dtype=np.float64)
    ) / 2.0
    events = []
    for i0, i1 in hysteresis_spans(both, BLINK_CLOSE, BLINK_REOPEN, below=True):
        dur_ms = _frame_span(t, i0, i1) * 1000.0
        if dur_ms < BLINK_MIN_MS:
            continue
        k = i0 + int(np.nanargmin(both[i0 : i1 + 1]))
        kind = "blink" if dur_ms <= BLINK_MAX_MS else "long_closure"
        events.append(Event(kind, t[i0], _end(t, i1), t[k], float(1.0 - both[k])))
    return events


def perclos(open_left, open_right, valid) -> float | None:
    """Share of valid frames with the eyes at least 80% closed (percent)."""
    both = (
        np.asarray(open_left, dtype=np.float64) + np.asarray(open_right, dtype=np.float64)
    ) / 2.0
    ok = np.asarray(valid, dtype=bool) & np.isfinite(both)
    if not ok.any():
        return None
    return float(100.0 * np.mean(both[ok] <= PERCLOS_OPENNESS))


# ── Expression ───────────────────────────────────────────────────────────────

#: Blendshapes that move with expression (not gaze or blinking), for the
#: expressivity index.
EXPRESSIVE = (
    "browDownLeft",
    "browDownRight",
    "browInnerUp",
    "browOuterUpLeft",
    "browOuterUpRight",
    "cheekSquintLeft",
    "cheekSquintRight",
    "mouthSmileLeft",
    "mouthSmileRight",
    "mouthFrownLeft",
    "mouthFrownRight",
    "mouthPucker",
    "mouthFunnel",
    "mouthPressLeft",
    "mouthPressRight",
    "mouthStretchLeft",
    "mouthStretchRight",
    "noseSneerLeft",
    "noseSneerRight",
    "jawOpen",
)

SMILE_ON, SMILE_OFF = 0.5, 0.35
DUCHENNE_CHEEK = 0.3  # cheek raise (AU6) during a smile (AU12)
BROW_ON, BROW_OFF = 0.5, 0.35
SMILE_MIN_MS = 300.0
BROW_MIN_MS = 100.0


def channel(shapes: dict, *names) -> np.ndarray:
    """Mean of the named blendshape series (each ``(T,)``)."""
    return np.mean([np.asarray(shapes[n], dtype=np.float64) for n in names], axis=0)


def detect_expression_events(shapes: dict, times_s) -> list[Event]:
    """Smiles, Duchenne smiles and brow raises.

    * **Smile** (AU12, lip corner puller): ``mouthSmileLeft/Right`` above
      :data:`SMILE_ON` for at least :data:`SMILE_MIN_MS`.
    * **Duchenne smile**: a smile during which the cheeks rise too (AU6,
      ``cheekSquint``), the marker of a felt rather than a polite smile
      (Ekman, Davidson and Friesen, 1990). Reported in addition to the smile.
    * **Brow raise** (AU1+2): ``browInnerUp`` with ``browOuterUp``. Short
      ones (under 600 ms) are the "eyebrow flash" of greeting and emphasis.
    """
    t = np.asarray(times_s, dtype=np.float64)
    events = []
    smile = channel(shapes, "mouthSmileLeft", "mouthSmileRight")
    cheek = channel(shapes, "cheekSquintLeft", "cheekSquintRight")
    for i0, i1 in hysteresis_spans(smile, SMILE_ON, SMILE_OFF):
        if _frame_span(t, i0, i1) * 1000.0 < SMILE_MIN_MS:
            continue
        k = i0 + int(np.nanargmax(smile[i0 : i1 + 1]))
        events.append(Event("smile", t[i0], _end(t, i1), t[k], float(smile[k])))
        if np.nanmax(cheek[i0 : i1 + 1]) >= DUCHENNE_CHEEK:
            events.append(Event("duchenne_smile", t[i0], _end(t, i1), t[k], float(smile[k])))
    brow = (
        np.asarray(shapes["browInnerUp"], dtype=np.float64)
        + channel(shapes, "browOuterUpLeft", "browOuterUpRight")
    ) / 2.0
    for i0, i1 in hysteresis_spans(brow, BROW_ON, BROW_OFF):
        dur = _frame_span(t, i0, i1) * 1000.0
        if dur < BROW_MIN_MS:
            continue
        k = i0 + int(np.nanargmax(brow[i0 : i1 + 1]))
        events.append(
            Event(
                "brow_raise", t[i0], _end(t, i1), t[k], float(brow[k]), {"flash": bool(dur < 600.0)}
            )
        )
    return events


def expressivity(shapes: dict, valid) -> np.ndarray:
    """How much the face moves, per frame: the mean of the
    :data:`EXPRESSIVE` blendshapes above this person's neutral level (their
    10th percentile over the recording). 0 at rest."""
    ok = np.asarray(valid, dtype=bool)
    rows = []
    for name in EXPRESSIVE:
        x = np.asarray(shapes[name], dtype=np.float64)
        rest = np.nanpercentile(x[ok], 10) if ok.any() else 0.0
        rows.append(np.clip(x - rest, 0.0, None))
    out = np.mean(rows, axis=0)
    out[~ok] = np.nan
    return out


# ── Head ─────────────────────────────────────────────────────────────────────

#: A swing counts towards a nod or shake from this many degrees.
GESTURE_MIN_DEG = 4.0
#: Each half-swing (down, or up) takes this long, seconds.
SWING_MIN_S, SWING_MAX_S = 0.15, 1.0
#: The other axis must stay this much quieter, so a nod is not a look around.
GESTURE_DOMINANCE = 1.5
#: A head movement reverses only after coming back this far, degrees.
ZIGZAG_DEG = 1.5


def detrend(angle_deg, times_s, window_s: float = 2.0) -> np.ndarray:
    """An angle minus its rolling median: the movement around the current
    head position, without slow drifts (turning towards someone)."""
    base = rolling_percentile(angle_deg, times_s, window_s, 50.0)
    return np.asarray(angle_deg, dtype=np.float64) - base


def _swings(
    x: np.ndarray, t: np.ndarray, reversal: float = ZIGZAG_DEG
) -> list[tuple[int, int, float]]:
    """Alternating extremes of a detrended angle: ``(index_from, index_to,
    amplitude)`` for each move from one turning point to the next.

    A turning point counts only once the angle has come back by ``reversal``
    degrees (a zig-zag filter), so landmark jitter of a fraction of a degree
    cannot split one swing into many small ones.
    """
    idx = np.flatnonzero(np.isfinite(x))
    if idx.size < 3:
        return []
    # At rest until the angle has spanned ``reversal`` (whatever its speed);
    # the first swing then starts at the extreme it moved away from, or more
    # exactly at the last frame still near that extreme (movement onset).
    turning: list[int] = []
    lo = hi = ext = int(idx[0])
    direction = 0.0
    for k in idx[1:]:
        k = int(k)
        if direction == 0.0:
            if x[k] < x[lo]:
                lo = k
            if x[k] > x[hi]:
                hi = k
            if x[hi] - x[lo] < reversal:
                continue
            start = lo if k == hi else hi
            direction = 1.0 if start == lo else -1.0
            near = [m for m in idx if start <= m <= k and abs(x[m] - x[start]) <= reversal / 3.0]
            turning.append(int(near[-1]))
            ext = k
            continue
        if (x[k] - x[ext]) * direction > 0.0:
            ext = k
        elif (x[ext] - x[k]) * direction >= reversal:
            turning.append(ext)
            direction = -direction
            ext = k
    if not turning:
        return []
    if ext != turning[-1]:
        turning.append(ext)
    return [(i, j, abs(float(x[j] - x[i]))) for i, j in zip(turning, turning[1:], strict=False)]


def detect_head_gestures(yaw_deg, pitch_deg, times_s) -> list[Event]:
    """Nods (pitch) and shakes (yaw).

    A gesture is at least two consecutive swings (down-up, or left-right) of
    at least :data:`GESTURE_MIN_DEG`, each taking :data:`SWING_MIN_S` to
    :data:`SWING_MAX_S`, while the other axis moves at most
    1/:data:`GESTURE_DOMINANCE` as much.
    """
    t = np.asarray(times_s, dtype=np.float64)
    yaw = detrend(yaw_deg, t)
    pitch = detrend(pitch_deg, t)
    events = []
    for kind, main, other in (("nod", pitch, yaw), ("shake", yaw, pitch)):
        run: list[tuple[int, int, float]] = []
        for sw in _swings(main, t) + [(0, 0, -1.0)]:
            i, j, amp = sw
            seg = other[i : j + 1]
            # Peak-to-peak on both axes, so a diagonal movement (equal on
            # both) is neither a nod nor a shake.
            other_amp = (
                float(np.nanmax(seg) - np.nanmin(seg))
                if amp > 0 and np.isfinite(seg).any()
                else 0.0
            )
            good = (
                amp >= GESTURE_MIN_DEG
                and SWING_MIN_S <= t[j] - t[i] <= SWING_MAX_S
                and other_amp * GESTURE_DOMINANCE <= amp
            )
            if good:
                run.append(sw)
                continue
            if len(run) >= 2:
                i0, i1 = run[0][0], run[-1][1]
                events.append(
                    Event(
                        kind,
                        t[i0],
                        _end(t, i1),
                        t[run[0][1]],
                        float(max(a for _, _, a in run)),
                        {"swings": len(run)},
                    )
                )
            run = []
    return sorted(events, key=lambda e: e.start_s)


def smooth(values, times_s, window_s: float = 0.1) -> np.ndarray:
    """Centred moving mean over ``window_s`` ignoring ``nan`` (and ``nan``
    where the window holds no finite value): takes the frame-to-frame
    landmark jitter out of the head angles."""
    v = np.asarray(values, dtype=np.float64)
    t = np.asarray(times_s, dtype=np.float64)
    finite = np.isfinite(v)
    csum = np.concatenate([[0.0], np.cumsum(np.where(finite, v, 0.0))])
    ccount = np.concatenate([[0], np.cumsum(finite)])
    lo = np.searchsorted(t, t - window_s / 2.0, side="left")
    hi = np.searchsorted(t, t + window_s / 2.0, side="right")
    n = ccount[hi] - ccount[lo]
    with np.errstate(divide="ignore", invalid="ignore"):
        out = (csum[hi] - csum[lo]) / n
    out[(n == 0) | ~finite] = np.nan
    return out


def angular_speed(yaw_deg, pitch_deg, roll_deg, times_s) -> np.ndarray:
    """Head rotation speed, degrees per second (``nan`` across missing frames)."""
    t = np.asarray(times_s, dtype=np.float64)
    a = np.column_stack([yaw_deg, pitch_deg, roll_deg]).astype(np.float64)
    out = np.full(len(t), np.nan)
    if len(t) > 1:
        d = np.linalg.norm(np.diff(a, axis=0), axis=1) / np.maximum(np.diff(t), 1e-6)
        out[1:] = d
    return out


# ── Summary ──────────────────────────────────────────────────────────────────


def summarise(events: list[Event], times_s, valid, open_left, open_right, expr, speed) -> dict:
    """Per-recording numbers over the time the face was measured."""
    t = np.asarray(times_s, dtype=np.float64)
    ok = np.asarray(valid, dtype=bool)
    step = float(np.median(np.diff(t))) if len(t) > 1 else 0.0
    seen_s = float(ok.sum() * step)
    minutes = seen_s / 60.0 if seen_s > 0 else float("nan")

    def of(kind):
        return [e for e in events if e.kind == kind]

    blinks = of("blink")
    durations = [e.duration_ms for e in blinks]
    # Intervals between consecutive blinks, skipping any that span frames
    # without a face (a look away would add its whole length).
    lost = np.concatenate([[0], np.cumsum(~ok)])
    first = np.searchsorted(t, [e.start_s for e in blinks])
    ibi = np.array(
        [
            b.start_s - a.start_s
            for a, b, ia, ib in zip(blinks, blinks[1:], first, first[1:], strict=False)
            if lost[ib] == lost[ia]
        ]
    )
    smiles = of("smile")

    def r(x, n=2):
        return None if x is None or (isinstance(x, float) and math.isnan(x)) else round(float(x), n)

    return {
        "face_seen_s": round(seen_s, 2),
        "face_seen_pct": round(100.0 * ok.mean(), 1) if len(ok) else 0.0,
        "blinks": {
            "count": len(blinks),
            "per_minute": r(len(blinks) / minutes),
            "median_duration_ms": r(np.median(durations)) if durations else None,
            "mean_interval_s": r(np.mean(ibi)) if ibi.size else None,
            "long_closures": len(of("long_closure")),
            "perclos_pct": r(perclos(open_left, open_right, ok)),
        },
        "expression": {
            "smiles": len(smiles),
            "duchenne_smiles": len(of("duchenne_smile")),
            "smiling_pct": r(100.0 * sum(e.duration_ms for e in smiles) / 1000.0 / seen_s)
            if seen_s
            else None,
            "brow_raises": len(of("brow_raise")),
            "brow_flashes": sum(1 for e in of("brow_raise") if e.extra.get("flash")),
            "expressivity_mean": r(np.nanmean(expr[ok]), 4) if ok.any() else None,
        },
        "head": {
            "nods": len(of("nod")),
            "shakes": len(of("shake")),
            "mean_speed_deg_s": r(np.nanmean(speed[ok])) if ok.any() else None,
        },
    }
