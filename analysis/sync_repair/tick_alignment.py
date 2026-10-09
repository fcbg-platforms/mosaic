"""
Places every frame of a hardware-triggered recording on the GigE Action
Command tick that produced it, and builds one equal-length, frame-per-tick
plan for every camera over the window where all of them were running.

Why this exists alongside alignment.py: that module lines cameras up on host
*arrival* time (``elapsed_ns``), at a frame rate measured afterwards. Arrival
time carries transfer and scheduling jitter, and the rate is a guess. A
triggered rig knows better: since recordings log every tick
(``video/action_ticks.csv``, see src/video/action_tick_log.hpp), a frame can be
assigned to the trigger that caused it.

How a frame finds its tick
--------------------------
1. **Map the camera's clock onto the host clock — globally, not frame by
   frame.** A camera's hardware timestamps (``hw_timestamp_ns``) run on its
   own clock: not comparable across cameras, but with ~1 ms jitter and only
   ppm-level drift within one. Arrival time = trigger time + latency, and
   latency varies by only a few ms. So a robust straight-line fit of arrival
   time against hardware time over the whole recording (outliers — host
   stalls, a corrupt timestamp — rejected and refitted) gives each frame's
   trigger time up to one constant: the typical latency. That constant's
   position *within* a tick period is the circular mean of every frame's
   offset from its nearest tick, so each frame lands on a tick on its own
   merits. A glitch or a stall moves that frame alone — a frame-by-frame
   walk, tried first, let one bad interval shift everything after it.
   A frame without a hardware timestamp (0) is placed by its arrival time
   instead; a camera with none at all is placed by arrival time throughout.
   Period changes mid-recording need no special case: frames are matched to
   the real tick times in the log.
2. **Which tick, not just which phase — anchoring by latency.** Step 1 fixes
   each frame to the tick grid, but the latency could be one period or
   another. Trigger-to-arrival latency is near-identical across identically
   configured cameras, so every camera is shifted by whole ticks until its
   median latency matches the reference camera's (the one with most frames).
   A residual over a third of a period is reported as ``uncertain``.

   **Exposure.** Arrival also waits for the exposure itself, which auto
   exposure varies per camera (up to ~23 ms apart on the rig) and per frame.
   When every camera's timestamps file carries each frame's exposure
   (``exposure_us``), it is subtracted from the arrival time before any of
   this, leaving a latency that really is near-identical across cameras.
3. **Absolute anchor.** The readiness barrier arms every camera before tick 0
   is fired, so the earliest camera's first frame answers tick 0. Everyone is
   shifted together to make that so.

Pure: numpy only, no files, no OpenCV — run_sync_repair.py does the I/O.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace

import numpy as np

#: A camera whose latency still differs from the reference's by more than this
#: fraction of a period after anchoring is flagged ``uncertain``.
UNCERTAIN_FRACTION = 1.0 / 3.0

#: Start/stop differences up to this long are ordinary — a camera joining a few
#: triggers late, stopping a frame early — and are trimmed from every camera so
#: all start and end together. A camera starting or stopping further from the
#: rest than this has dropped out (unplugged, link lost): it must not cut the
#: other cameras' recording to its own length, so the others keep their full
#: span and it is MISSING for the part it was absent. Found on the rig
#: (2026-10-02): one camera unplugged and lost for good cost five healthy
#: cameras the last 27 s of a 44 s recording.
DROPOUT_TOLERANCE_S = 2.0

#: A camera's per-frame exposure is used only when at least this share of its
#: frames report one (the rest take its median); otherwise no camera is
#: corrected, since correcting some and not others would skew the latency
#: comparison more than it helps.
EXPOSURE_MIN_SHARE = 0.9

#: Clock-fit residuals beyond this many robust standard deviations (MAD-based)
#: are left out of the refit — a host stall makes a frame arrive late, and it
#: must not drag the line.
_OUTLIER_SIGMAS = 4.0


@dataclass
class TickCamera:
    """One camera's frames, in recorded order. ``index`` is the configured
    index (video_N.mp4's N). ``hw_ns`` is 0 for every frame whose hardware
    timestamp was unavailable."""

    index: int
    frame_ids: np.ndarray  # int64
    elapsed_ns: np.ndarray  # int64, host arrival time
    hw_ns: np.ndarray  # int64, camera clock; 0 = unavailable
    # int64, each frame's exposure time in ns; 0 = unavailable. None for
    # recordings made before timestamps files carried it.
    exposure_ns: np.ndarray | None = None


@dataclass
class CameraPlan:
    index: int
    # Per output tick: the frame_id to show — the frame that tick produced,
    # or, when ``missing``, the last real frame before it.
    frame_ids: np.ndarray
    missing: np.ndarray  # bool, same length
    method: str  # "trigger_ticks:hw_timestamp" | "trigger_ticks:arrival_interval"
    lead_in_trimmed: int = 0  # this camera's frames before the window
    tail_trimmed: int = 0  # ... and after it
    uncertain: bool = False
    # Trigger to arrival, less the exposure when the plan subtracted it.
    median_latency_ms: float | None = None
    # Output index where this camera first delivers, when it joined later than
    # DROPOUT_TOLERANCE_S after the others (missing before it); else None.
    joined_late_at: int | None = None
    # Output index after its last frame, when it stopped more than
    # DROPOUT_TOLERANCE_S before the others (missing from there); else None.
    dropped_out_at: int | None = None


@dataclass
class TickPlan:
    first_tick: int  # absolute tick index of output frame 0
    total_ticks: int
    tick_rate_fps: float  # median over the window
    min_tick_rate_fps: float  # the ticker re-paces itself; these show by how much
    max_tick_rate_fps: float
    duration_ms: float  # real time from the first output tick to one period past the last
    cameras: dict[int, CameraPlan] = field(default_factory=dict)
    # Whether each frame's exposure was subtracted from its arrival time.
    exposure_corrected: bool = False


# ── Tick lookup ──────────────────────────────────────────────────────────────


def _nearest_ticks(tick_times: np.ndarray, t_ns: np.ndarray) -> np.ndarray:
    """Index of the tick nearest each time in ``t_ns`` — extrapolated past
    either end of the log at the median period rather than clamped. Positions
    are only provisional until the whole-tick shifts in build_tick_plan(), and
    clamping a frame that provisionally sits one past the last tick merges it
    with its neighbour, losing it."""
    t_ns = np.asarray(t_ns, dtype=np.float64)
    n = len(tick_times)
    period = float(np.median(np.diff(tick_times)))
    j = np.searchsorted(tick_times, t_ns, side="left")
    jc = np.clip(j, 1, n - 1)
    left = tick_times[jc - 1]
    right = tick_times[jc]
    inside = np.where(np.abs(t_ns - left) <= np.abs(right - t_ns), jc - 1, jc)
    before = np.round((t_ns - tick_times[0]) / period)
    after = (n - 1) + np.round((t_ns - tick_times[-1]) / period)
    out = np.where(t_ns < tick_times[0], before, np.where(t_ns > tick_times[-1], after, inside))
    return out.astype(np.int64)


def _tick_time(tick_times: np.ndarray, k: np.ndarray) -> np.ndarray:
    """Time of tick ``k``, extrapolated at the median period past either end
    of the log (see _nearest_ticks())."""
    k = np.asarray(k, dtype=np.int64)
    n = len(tick_times)
    period = float(np.median(np.diff(tick_times)))
    kc = np.clip(k, 0, n - 1)
    return tick_times[kc] + (k - kc) * period


# ── Step 1: each frame onto the tick grid ───────────────────────────────────


def _robust_line(x: np.ndarray, y: np.ndarray) -> tuple[float, float]:
    """y ≈ a·x + b, refitted without outliers. x and y are float64 offsets
    (already centred near zero so the fit keeps nanosecond precision)."""
    keep = np.ones(len(x), dtype=bool)
    a, b = 1.0, float(np.median(y - x))
    for _ in range(3):
        if keep.sum() < 2:
            break
        a, b = np.polyfit(x[keep], y[keep], 1)
        resid = y - (a * x + b)
        mad = float(np.median(np.abs(resid[keep] - np.median(resid[keep]))))
        limit = max(_OUTLIER_SIGMAS * 1.4826 * mad, 2e6)  # never tighter than 2 ms
        new_keep = np.abs(resid - np.median(resid[keep])) <= limit
        if new_keep.sum() < 2 or np.array_equal(new_keep, keep):
            break
        keep = new_keep
    return float(a), float(b)


def _circular_phase(offsets_ns: np.ndarray, period_ns: float) -> float:
    """The typical offset of times from their nearest tick, in
    (-period/2, period/2] — a circular mean, since an offset of +19 ms and one
    of -19 ms at a 40 ms period are neighbours, not opposites."""
    ang = 2.0 * np.pi * offsets_ns / period_ns
    mean = np.arctan2(np.mean(np.sin(ang)), np.mean(np.cos(ang)))
    return float(mean / (2.0 * np.pi) * period_ns)


def grid_positions(
    tick_times: np.ndarray, cam: TickCamera, period_ns: float
) -> tuple[np.ndarray, str]:
    """Tick index for every frame of one camera, correct up to one whole-tick
    shift shared by all its frames (resolved by the caller). Returns
    (ticks, method)."""
    elapsed = cam.elapsed_ns.astype(np.float64)
    hw = cam.hw_ns
    has_hw = hw != 0

    # Arrival-based estimate of each frame's tick: the phase of arrival times
    # against the grid gives the latency modulo a period.
    near = _nearest_ticks(tick_times, elapsed)
    arr_phase = _circular_phase(elapsed - _tick_time(tick_times, near), period_ns)
    by_arrival = _nearest_ticks(tick_times, elapsed - arr_phase)

    if has_hw.sum() < 2:
        return by_arrival, "trigger_ticks:arrival_interval"

    # Hardware clock -> host clock, fitted on the frames that have it.
    x0 = float(hw[has_hw][0])
    y0 = float(elapsed[has_hw][0])
    x = hw[has_hw].astype(np.float64) - x0
    y = elapsed[has_hw] - y0
    a, b = _robust_line(x, y)
    mapped = a * (hw.astype(np.float64) - x0) + b + y0  # host-clock time per frame
    near = _nearest_ticks(tick_times, mapped[has_hw])
    hw_phase = _circular_phase(mapped[has_hw] - _tick_time(tick_times, near), period_ns)
    by_hw = _nearest_ticks(tick_times, mapped - hw_phase)

    # Frames without a hardware timestamp fall back to arrival, shifted to
    # agree with the hardware-placed frames (the two estimates can differ by
    # whole ticks, since each phase is only known modulo a period).
    if not has_hw.all():
        shift = int(np.round(np.median(by_hw[has_hw] - by_arrival[has_hw])))
        by_hw = np.where(has_hw, by_hw, by_arrival + shift)
    return by_hw, "trigger_ticks:hw_timestamp"


def subtract_exposure(cameras: list[TickCamera]) -> tuple[list[TickCamera], bool]:
    """Each camera with its exposure taken off every frame's arrival time, and
    True; or the cameras unchanged and False, unless every camera reports an
    exposure for at least EXPOSURE_MIN_SHARE of its frames. A frame without
    one takes its camera's median."""
    out: list[TickCamera] = []
    for cam in cameras:
        exp = cam.exposure_ns
        if exp is None or len(exp) != len(cam.elapsed_ns) or len(exp) == 0:
            return cameras, False
        known = exp > 0
        if known.mean() < EXPOSURE_MIN_SHARE:
            return cameras, False
        filled = np.where(known, exp, int(np.median(exp[known])))
        out.append(replace(cam, elapsed_ns=cam.elapsed_ns - filled.astype(np.int64)))
    return out, True


def _latencies(tick_times: np.ndarray, elapsed: np.ndarray, ticks: np.ndarray) -> np.ndarray:
    return elapsed.astype(np.float64) - _tick_time(tick_times, ticks)


# ── Whole-session plan ───────────────────────────────────────────────────────


def build_tick_plan(tick_times_ns: np.ndarray, cameras: list[TickCamera]) -> TickPlan | None:
    """Equal-length frame-per-tick plan for every camera, or None when the
    ticks cannot be used (fewer than two ticks, or no camera with at least two
    frames) — the caller then falls back to arrival-time alignment.

    ``cameras`` are the cameras in the Action group only; a free-running
    camera never followed these ticks and must not be forced onto them.
    Cameras with fewer than two frames are left out of the plan; the caller
    reports them."""
    tick_times = np.asarray(tick_times_ns, dtype=np.float64)
    usable = [c for c in cameras if len(c.frame_ids) >= 2]
    if len(tick_times) < 2 or not usable:
        return None
    usable, exposure_corrected = subtract_exposure(usable)
    period = float(np.median(np.diff(tick_times)))
    if not period > 0:
        return None

    positions: dict[int, tuple[np.ndarray, str]] = {
        c.index: grid_positions(tick_times, c, period) for c in usable
    }

    # Step 2: whole-tick shifts so every camera's latency matches the
    # reference's.
    ref = max(usable, key=lambda c: len(c.frame_ids))
    ref_lat = float(np.median(_latencies(tick_times, ref.elapsed_ns, positions[ref.index][0])))
    shifted: dict[int, np.ndarray] = {}
    for cam in usable:
        ticks, _ = positions[cam.index]
        lat = float(np.median(_latencies(tick_times, cam.elapsed_ns, ticks)))
        shifted[cam.index] = ticks + int(round((lat - ref_lat) / period))

    # Step 3: the earliest camera's first frame answers tick 0.
    g = min(int(t[0]) for t in shifted.values())
    for idx in shifted:
        shifted[idx] = shifted[idx] - g

    placed: dict[int, tuple[np.ndarray, str, float]] = {}
    for cam in usable:
        ticks = shifted[cam.index]
        valid = (ticks >= 0) & (ticks < len(tick_times))
        lat = float(np.median(_latencies(tick_times, cam.elapsed_ns[valid], ticks[valid])))
        placed[cam.index] = (ticks, positions[cam.index][1], lat)
    ref_final = placed[ref.index][2]

    # The window: where every camera was recording, give or take ordinary
    # start/stop differences — see DROPOUT_TOLERANCE_S. A camera outside the
    # tolerance does not move the window; it is missing for that part.
    tol = max(1, int(round(DROPOUT_TOLERANCE_S * 1e9 / period)))
    firsts = [int(t[0]) for t, *_ in placed.values()]
    lasts = [int(t[-1]) for t, *_ in placed.values()]
    first = max(f for f in firsts if f <= min(firsts) + tol)
    last = min(v for v in lasts if v >= max(lasts) - tol)
    first = max(first, 0)
    last = min(last, len(tick_times) - 1)
    if last < first:
        return None
    total = last - first + 1
    window = tick_times[first : last + 1]
    if len(window) >= 2:
        rates = 1e9 / np.diff(window)
        rate, rmin, rmax = float(np.median(rates)), float(rates.min()), float(rates.max())
        span_ms = (float(window[-1] - window[0]) + float(np.median(np.diff(window)))) / 1e6
    else:
        rate = rmin = rmax = 1e9 / period
        span_ms = period / 1e6

    plan = TickPlan(
        first_tick=first,
        total_ticks=total,
        tick_rate_fps=rate,
        min_tick_rate_fps=rmin,
        max_tick_rate_fps=rmax,
        duration_ms=span_ms,
        exposure_corrected=exposure_corrected,
    )
    for cam in usable:
        ticks, method, lat = placed[cam.index]
        out_ids = np.empty(total, dtype=np.int64)
        missing = np.ones(total, dtype=bool)
        # The frame each output tick shows: its own, or the last real one
        # before it. A camera that joined late has none before its first
        # frame: those ticks show its first frame, tagged MISSING.
        ptr = 0
        last_real = int(cam.frame_ids[0])
        for out in range(total):
            tick = first + out
            while ptr < len(ticks) and ticks[ptr] < tick:
                last_real = int(cam.frame_ids[ptr])
                ptr += 1
            if ptr < len(ticks) and ticks[ptr] == tick:
                last_real = int(cam.frame_ids[ptr])
                out_ids[out] = last_real
                missing[out] = False
                ptr += 1
                # Two frames on one tick (should not happen with triggers):
                # keep the first rather than shift the timeline.
                while ptr < len(ticks) and ticks[ptr] == tick:
                    ptr += 1
            else:
                out_ids[out] = last_real
        plan.cameras[cam.index] = CameraPlan(
            index=cam.index,
            frame_ids=out_ids,
            missing=missing,
            method=method,
            lead_in_trimmed=int(np.sum(ticks < first)),
            tail_trimmed=int(np.sum(ticks > last)),
            uncertain=abs(lat - ref_final) > UNCERTAIN_FRACTION * period,
            median_latency_ms=lat / 1e6,
            joined_late_at=int(ticks[0]) - first if int(ticks[0]) > first else None,
            dropped_out_at=int(ticks[-1]) - first + 1 if int(ticks[-1]) < last else None,
        )
    return plan


def gap_ranges(missing: np.ndarray) -> list[tuple[int, int]]:
    """Inclusive (start, end) output-index ranges where ``missing`` is True."""
    out: list[tuple[int, int]] = []
    start = None
    for i, m in enumerate(missing):
        if m and start is None:
            start = i
        elif not m and start is not None:
            out.append((start, i - 1))
            start = None
    if start is not None:
        out.append((start, len(missing) - 1))
    return out
