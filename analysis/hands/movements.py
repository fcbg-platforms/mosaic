"""Hand movements (reaches, grasps, other moves) found in a hand's wrist
speed, with the measures the reaching and grasping literature uses: peak
speed and when it comes, movement units, SPARC smoothness, straightness and
the maximum grip aperture; and per person, which hand moves and when both
do. See docs/math/hands.rst.

Speeds are in hand lengths per second (:mod:`hands.kinematics`): for a palm
of about 10 cm, 1 hand length per second is about 0.1 m/s.
"""

from __future__ import annotations

import numpy as np

#: A movement is where the wrist moves faster than this...
ON_HL_S = 0.5
#: ...peaks at this or faster...
MIN_PEAK_HL_S = 1.5
#: ...and lasts this long.
MIN_DURATION_S = 0.2
#: A movement unit (after von Hofsten, 1991: one speeding up and one
#: slowing down): a speed peak this far above the lowest speed on each side
#: of it, about 20 mm/s for a 10 cm palm.
UNIT_RISE_HL_S = 0.2
#: SPARC: the speed spectrum up to this, and down to this share of its peak.
SPARC_MAX_HZ = 10.0
SPARC_AMPLITUDE = 0.05

MOVEMENT_COLUMNS = (
    "subject_id",
    "side",
    "start_ms",
    "end_ms",
    "duration_s",
    "peak_speed_hl_s",
    "time_to_peak_pct",
    "mean_speed_hl_s",
    "path_hl",
    "straightness",
    "movement_units",
    "sparc",
    "max_grip_aperture_palm",
    "time_to_max_aperture_pct",
)


def stretches(time_ms: np.ndarray, values: np.ndarray, rate: float) -> list:
    """``[(start, end)]`` index ranges of finite values on an unbroken
    grid: a NaN, or a jump in time (between two of a hand's runs), ends one."""
    ok = np.isfinite(values)
    new = np.r_[True, np.diff(time_ms) > 1500.0 / rate]
    out, start = [], None
    for i in range(len(values)):
        if start is not None and (not ok[i] or new[i]):
            out.append((start, i))
            start = None
        if ok[i] and start is None:
            start = i
    if start is not None:
        out.append((start, len(values)))
    return out


def movement_units(speed: np.ndarray) -> int:
    """Speed peaks that rise at least :data:`UNIT_RISE_HL_S` above the
    lowest speed on each side of them, before the next higher peak (their
    topographic prominence): 1 for a single smooth movement."""
    from scipy.signal import find_peaks

    if len(speed) < 3:
        return 1
    padded = np.r_[0.0, speed, 0.0]  # a movement starts and ends slow
    peaks, _ = find_peaks(padded, prominence=UNIT_RISE_HL_S)
    return max(1, len(peaks))


def sparc(speed: np.ndarray, rate: float) -> float:
    """Spectral arc length (Balasubramanian, Melendez-Calderon, Roby-Brami
    and Burdet, 2015): minus the length of the speed's normalised magnitude
    spectrum, up to :data:`SPARC_MAX_HZ` (or the Nyquist frequency) and
    down to :data:`SPARC_AMPLITUDE` of its peak. Closer to 0 is smoother:
    -1.41 for one minimum-jerk reach, at any frame rate (as the authors'
    reference code gives); lower with more stops and starts."""
    n = len(speed)
    if n < 2 or not np.any(speed > 0):
        return 0.0
    nfft = int(2 ** (np.ceil(np.log2(max(n, 2))) + 4))
    f = np.arange(nfft) * rate / nfft
    m = np.abs(np.fft.fft(speed, nfft))
    m = m / m.max() if m.max() > 0 else m
    keep = f <= min(SPARC_MAX_HZ, rate / 2.0)
    f, m = f[keep], m[keep]
    above = np.flatnonzero(m >= SPARC_AMPLITUDE)
    f, m = f[above[0] : above[-1] + 1], m[above[0] : above[-1] + 1]
    if len(f) < 2:
        return 0.0
    df = np.diff(f) / (f[-1] - f[0])
    return float(-np.sum(np.sqrt(df**2 + np.diff(m) ** 2)))


def _widen(speed: np.ndarray, s0: int, s1: int, a: int, b: int) -> tuple:
    """``[s0, s1)`` widened to the speed's nearest low point on each side,
    within ``[a, b)``: the whole speed profile of the movement, from rest
    (or its slowest) to rest, for the measures of its shape."""
    while s0 > a and speed[s0 - 1] < speed[s0]:
        s0 -= 1
    while s1 < b and speed[s1] < speed[s1 - 1]:
        s1 += 1
    return s0, s1


def find_movements(series: dict, rate: float) -> tuple:
    """One hand's movements, each a dict of :data:`MOVEMENT_COLUMNS` (but
    the person and side), from :func:`hands.kinematics.hand_series`; and the
    ones left out because the measurement starts or ends during them (the
    hand appears already moving, or is lost mid-movement), whose duration,
    peak and shape would be wrong: ``(movements, cut_off)``, each cut-off
    one only its ``start_ms`` and ``end_ms`` (what is seen of it), for how
    the hands were used.

    Duration, speeds, path, straightness and grip aperture are over the part
    above :data:`ON_HL_S`; movement units and SPARC over the whole speed
    profile, widened to its low points, so that a slow movement (starting
    and ending near the threshold) is not cut off on a slope."""
    t = series["time_ms"]
    speed = series["wrist_speed_hl_s"]
    x, y = series["wrist_x_px"], series["wrist_y_px"]
    aperture = series.get("grip_aperture_palm")
    out, cut_off = [], []
    for a, b in stretches(t, speed, rate):
        moving = speed[a:b] > ON_HL_S
        edges = np.flatnonzero(np.diff(np.r_[0, moving.astype(int), 0]))
        for s0, s1 in zip(edges[::2] + a, edges[1::2] + a, strict=True):
            v = speed[s0:s1]
            duration = (t[s1 - 1] - t[s0]) / 1000.0
            if duration < MIN_DURATION_S or v.max() < MIN_PEAK_HL_S:
                continue
            if s0 == a or s1 == b:
                cut_off.append({"start_ms": float(t[s0]), "end_ms": float(t[s1 - 1])})
                continue
            w0, w1 = _widen(speed, s0, s1, a, b)
            whole = speed[w0:w1]
            peak = int(np.argmax(v))
            path = float(np.sum(v) / rate)
            # Straightness: the wrist's straight-line distance over its path,
            # both in pixels (the palm unit cancels).
            px_path = float(np.sum(np.hypot(np.diff(x[s0:s1]), np.diff(y[s0:s1]))))
            px_line = float(np.hypot(x[s1 - 1] - x[s0], y[s1 - 1] - y[s0]))
            m = {
                "start_ms": round(float(t[s0]), 1),
                "end_ms": round(float(t[s1 - 1]), 1),
                "duration_s": round(duration, 3),
                "peak_speed_hl_s": round(float(v.max()), 3),
                "time_to_peak_pct": round(100.0 * peak / max(1, len(v) - 1), 1),
                "mean_speed_hl_s": round(float(v.mean()), 3),
                "path_hl": round(path, 3),
                "straightness": round(px_line / px_path, 3) if px_path > 0 else None,
                "movement_units": movement_units(whole),
                "sparc": round(sparc(whole, rate), 3),
                "max_grip_aperture_palm": None,
                "time_to_max_aperture_pct": None,
            }
            if aperture is not None and np.isfinite(aperture[s0:s1]).any():
                ap = aperture[s0:s1]
                k = int(np.nanargmax(ap))
                m["max_grip_aperture_palm"] = round(float(ap[k]), 3)
                m["time_to_max_aperture_pct"] = round(100.0 * k / max(1, len(ap) - 1), 1)
            out.append(m)
    return out, cut_off


def hand_summary(movements: list, cut_off: int = 0) -> dict:
    """A hand's movements in a few numbers: how many, how many were left out
    as cut off, and the medians."""

    def median(key, digits=2):
        v = [m[key] for m in movements if m[key] is not None]
        return round(float(np.median(v)), digits) if v else None

    return {
        "movements": len(movements),
        "cut_off": cut_off,
        "duration_median_s": median("duration_s"),
        "peak_speed_median_hl_s": median("peak_speed_hl_s"),
        "time_to_peak_median_pct": median("time_to_peak_pct", 1),
        "movement_units_median": median("movement_units", 1),
        "sparc_median": median("sparc"),
        "straightness_median": median("straightness"),
        "max_grip_aperture_median_palm": median("max_grip_aperture_palm"),
    }


def person_summary(left: list, right: list) -> dict:
    """How a person used their two hands, from all of each hand's movements
    (cut-off ones too: they are still that hand moving): the laterality
    index, (right - left) / (right + left) movements, from -1 (all left) to
    1 (all right); and how long both hands moved at once, in seconds and as
    a share of the time either did."""
    n_l, n_r = len(left), len(right)
    spans = lambda ms: [(m["start_ms"], m["end_ms"]) for m in ms]  # noqa: E731
    both = 0.0
    for a0, a1 in spans(left):
        for b0, b1 in spans(right):
            both += max(0.0, min(a1, b1) - max(a0, b0))
    either = sum(e - s for s, e in spans(left)) + sum(e - s for s, e in spans(right)) - both
    return {
        "laterality_index": round((n_r - n_l) / (n_r + n_l), 2) if n_l + n_r else None,
        "both_moving_s": round(both / 1000.0, 2),
        "both_moving_share": round(both / either, 2) if either > 0 else None,
    }
