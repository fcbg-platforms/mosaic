"""
Tests for sync_repair/tick_alignment.py — placing every frame on the trigger
tick that produced it. Synthetic rigs with known ground truth: each test says
which ticks every camera really answered, and checks the plan recovers exactly
that, including which frames are missing.
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent.parent))

from sync_repair.tick_alignment import TickCamera, build_tick_plan, clock_segments, gap_ranges

PERIOD = 40_000_000  # 25 fps
LATENCY = 45_000_000  # trigger -> arrival; longer than a period on purpose


def _ticks(n, period=PERIOD, start=1_000_000_000):
    return np.array([start + i * period for i in range(n)], dtype=np.int64)


def _camera(
    index,
    tick_times,
    answered,
    *,
    hw_offset=7_777_000_000,
    latency=LATENCY,
    jitter=None,
    hw=True,
    seed=0,
):
    """A camera that produced one frame on each tick in `answered`. Its
    hardware clock is offset arbitrarily (not comparable across cameras) and
    its arrival time is the tick plus a latency with optional jitter."""
    rng = np.random.default_rng(seed + index)
    answered = list(answered)
    t = tick_times[answered].astype(np.int64)
    arr_jitter = rng.integers(-jitter, jitter + 1, len(t)) if jitter else 0
    hw_jitter = rng.integers(-1_000_000, 1_000_001, len(t)) if jitter else 0
    return TickCamera(
        index=index,
        frame_ids=np.arange(1, len(t) + 1, dtype=np.int64),
        elapsed_ns=(t + latency + arr_jitter).astype(np.int64),
        hw_ns=(
            (t + hw_offset + index * 123_456_789 + hw_jitter).astype(np.int64)
            if hw
            else np.zeros(len(t), dtype=np.int64)
        ),
    )


def _shift(plan, truth_first_tick_of_cam0=0):
    """Absolute tick labels are only known up to one shift shared by every
    camera (the anchor is the latest tick before the reference's first frame
    arrived, so a latency longer than a period labels everything one tick
    late). Only relative alignment matters; tests undo the shared shift using
    camera 0, which answers every tick from `truth_first_tick_of_cam0`."""
    c0 = plan.cameras[0]
    first_real = next(i for i in range(plan.total_ticks) if not c0.missing[i])
    lead = c0.lead_in_trimmed
    return plan.first_tick + first_real - lead - truth_first_tick_of_cam0


def _real_ticks(plan, index):
    cam = plan.cameras[index]
    s = _shift(plan)
    return [plan.first_tick + i - s for i in range(plan.total_ticks) if not cam.missing[i]]


# ── Equal counts and the common window ──────────────────────────────────────


def test_a_perfect_rig_has_equal_counts_and_nothing_missing():
    ticks = _ticks(200)
    cams = [_camera(i, ticks, range(200)) for i in range(6)]
    plan = build_tick_plan(ticks, cams)
    assert plan is not None
    assert plan.total_ticks == 200
    for c in plan.cameras.values():
        assert len(c.frame_ids) == 200
        assert not c.missing.any()
        assert c.method == "trigger_ticks:hw_timestamp"
        assert not c.uncertain
    assert abs(plan.tick_rate_fps - 25.0) < 1e-6


def test_a_late_camera_trims_the_others_lead_in_not_its_own_frames():
    # Camera 2 (index 1) joined 7 ticks late — the ~260 ms seen on the rig.
    ticks = _ticks(150)
    cams = [
        _camera(0, ticks, range(150)),
        _camera(1, ticks, range(7, 150)),
        _camera(2, ticks, range(150)),
    ]
    plan = build_tick_plan(ticks, cams)
    assert plan.first_tick - _shift(plan) == 7
    assert plan.total_ticks == 143
    assert plan.cameras[0].lead_in_trimmed == 7
    assert plan.cameras[1].lead_in_trimmed == 0
    for c in plan.cameras.values():
        assert not c.missing.any()


def test_different_stop_times_trim_the_tail():
    ticks = _ticks(120)
    cams = [_camera(0, ticks, range(120)), _camera(1, ticks, range(115))]
    plan = build_tick_plan(ticks, cams)
    assert plan.total_ticks == 115
    assert plan.cameras[0].tail_trimmed == 5


# ── Missing frames ───────────────────────────────────────────────────────────


def test_dropped_frames_are_missing_on_exactly_their_ticks():
    ticks = _ticks(100)
    dropped = {30, 31, 32, 60}
    cams = [
        _camera(0, ticks, range(100)),
        _camera(1, ticks, [k for k in range(100) if k not in dropped]),
    ]
    plan = build_tick_plan(ticks, cams)
    missing_ticks = {
        plan.first_tick + i - _shift(plan) for i, m in enumerate(plan.cameras[1].missing) if m
    }
    assert missing_ticks == dropped
    first = plan.first_tick - _shift(plan)
    assert gap_ranges(plan.cameras[1].missing) == [
        (30 - first, 32 - first),
        (60 - first, 60 - first),
    ]
    # A missing tick shows the last real frame before it.
    assert plan.cameras[1].frame_ids[30 - first] == plan.cameras[1].frame_ids[29 - first]
    assert not plan.cameras[0].missing.any()


def test_real_frames_land_on_the_ticks_that_produced_them():
    ticks = _ticks(80)
    answered = [k for k in range(80) if k % 9 != 4]
    cams = [_camera(0, ticks, range(80)), _camera(1, ticks, answered)]
    plan = build_tick_plan(ticks, cams)
    assert _real_ticks(plan, 1) == answered


# ── Robustness ───────────────────────────────────────────────────────────────


def test_a_period_change_mid_recording_is_followed():
    # The ticker re-paces itself as cameras' measured rates improve.
    first = _ticks(60)
    second = first[-1] + np.arange(1, 61, dtype=np.int64) * 36_000_000
    ticks = np.concatenate([first, second])
    answered = [k for k in range(120) if k not in (70, 71)]
    cams = [_camera(0, ticks, range(120)), _camera(1, ticks, answered)]
    plan = build_tick_plan(ticks, cams)
    assert _real_ticks(plan, 1) == answered


def test_jitter_does_not_move_frames_between_ticks():
    ticks = _ticks(300)
    answered = [k for k in range(300) if k not in (100, 200, 201)]
    cams = [
        _camera(0, ticks, range(300), jitter=8_000_000),
        _camera(1, ticks, answered, jitter=8_000_000),
    ]
    plan = build_tick_plan(ticks, cams)
    assert _real_ticks(plan, 1) == answered


def test_cameras_with_different_latencies_are_still_aligned_and_flagged_only_when_far():
    ticks = _ticks(100)
    cams = [
        _camera(0, ticks, range(100)),
        _camera(1, ticks, range(3, 100), latency=LATENCY + 4_000_000),
    ]
    plan = build_tick_plan(ticks, cams)
    assert _real_ticks(plan, 1) == list(range(3, 100))
    assert not plan.cameras[1].uncertain


def test_without_hardware_timestamps_arrival_intervals_are_used():
    ticks = _ticks(90)
    answered = [k for k in range(90) if k != 45]
    cams = [_camera(0, ticks, range(90), hw=False), _camera(1, ticks, answered, hw=False)]
    plan = build_tick_plan(ticks, cams)
    assert plan.cameras[1].method == "trigger_ticks:arrival_interval"
    assert _real_ticks(plan, 1) == answered


def test_one_missing_hardware_timestamp_affects_nothing():
    # The grabber writes 0 when a single frame's chunk read fails. The first
    # version took the interval across it at face value and lost the rest of
    # the camera's recording.
    ticks = _ticks(100)
    cam1 = _camera(1, ticks, range(100))
    cam1.hw_ns[40] = 0
    plan = build_tick_plan(ticks, [_camera(0, ticks, range(100)), cam1])
    assert _real_ticks(plan, 1) == list(range(100))
    assert plan.total_ticks == 100


def test_a_corrupt_hardware_timestamp_moves_at_most_its_own_frame():
    ticks = _ticks(100)
    cam1 = _camera(1, ticks, range(100))
    cam1.hw_ns[50] += 3 * PERIOD // 2  # one bad value, not a clock step
    plan = build_tick_plan(ticks, [_camera(0, ticks, range(100)), cam1])
    real = _real_ticks(plan, 1)
    assert real[:50] == list(range(50))
    assert real[-49:] == list(range(51, 100))
    assert plan.cameras[1].missing.sum() <= 1


def test_a_host_stall_does_not_move_frames_placed_by_hardware_time():
    # Three frames arrive 70 ms late because the capture thread stalled. Their
    # hardware timestamps are right; their arrival times are not.
    ticks = _ticks(120)
    cam1 = _camera(1, ticks, range(120))
    cam1.elapsed_ns[60:63] += 70_000_000
    plan = build_tick_plan(ticks, [_camera(0, ticks, range(120)), cam1])
    assert _real_ticks(plan, 1) == list(range(120))


def test_camera_clock_drift_over_a_long_recording():
    # 50 ppm over 20 minutes is 60 ms — more than a period. The fit absorbs it.
    ticks = _ticks(30_000)
    cam1 = _camera(1, ticks, [k for k in range(30_000) if k != 25_000])
    cam1.hw_ns = (cam1.hw_ns.astype(np.float64) * (1 + 50e-6)).astype(np.int64)
    plan = build_tick_plan(ticks, [_camera(0, ticks, range(30_000)), cam1])
    missing = {plan.first_tick + i for i, m in enumerate(plan.cameras[1].missing) if m}
    assert missing == {25_000}


def test_the_output_reports_a_changing_tick_rate():
    first = _ticks(60)
    second = first[-1] + np.arange(1, 61, dtype=np.int64) * 36_000_000
    ticks = np.concatenate([first, second])
    plan = build_tick_plan(ticks, [_camera(0, ticks, range(120)), _camera(1, ticks, range(120))])
    assert plan.min_tick_rate_fps < 25.5 and plan.max_tick_rate_fps > 27.0
    # The real span, not frames / median rate.
    assert abs(plan.duration_ms - ((ticks[-1] - ticks[0]) / 1e6 + 40.0)) < 5.0


# ── When the ticks cannot be used ────────────────────────────────────────────


def test_unusable_input_returns_none_for_the_arrival_time_fallback():
    ticks = _ticks(50)
    assert build_tick_plan(ticks[:1], [_camera(0, ticks, range(50))]) is None
    assert build_tick_plan(ticks, [_camera(0, ticks, [3])]) is None
    assert build_tick_plan(ticks, []) is None


def test_gap_ranges():
    m = np.array([False, True, True, False, True, False, False, True], dtype=bool)
    assert gap_ranges(m) == [(1, 2), (4, 4), (7, 7)]
    assert gap_ranges(np.zeros(4, dtype=bool)) == []


def test_latency_longer_than_a_period_labels_ticks_exactly():
    # Exposure plus a full-frame transfer can exceed one 40 ms period. The
    # first version anchored a tick late and lost the last frame off the end
    # of the log; anchoring on "the first frame answers tick 0" is exact.
    ticks = _ticks(100)
    cams = [
        _camera(0, ticks, range(100), latency=95_000_000),
        _camera(1, ticks, range(2, 100), latency=95_000_000),
    ]
    plan = build_tick_plan(ticks, cams)
    assert plan.first_tick == 2
    assert plan.total_ticks == 98
    c1 = plan.cameras[1]
    assert [plan.first_tick + i for i in range(plan.total_ticks) if not c1.missing[i]] == list(
        range(2, 100)
    )


# ── The visible mark ─────────────────────────────────────────────────────────


def test_mark_missing_tags_the_corner_and_leaves_the_rest_and_the_input_alone():
    from sync_repair.marker import mark_missing, tag_geometry

    frame = np.full((1080, 1920, 3), 90, dtype=np.uint8)
    marked = mark_missing(frame)
    assert (frame == 90).all()  # input untouched
    box_w, box_h, _, _ = tag_geometry(1920, 1080)
    edge = marked[:2, :box_w]  # the tag's top edge, above the lettering
    assert (edge[..., 2] > 150).all() and (edge[..., 0] < 100).all()  # red (BGR)
    assert (marked[box_h + 5 :, :] == 90).all()  # nothing below the tag
    assert (marked[:, box_w + 5 :] == 90).all()  # nothing right of it
    assert box_h < 1080 // 10  # small: plugins still see the image


def test_mark_missing_fits_a_tiny_frame():
    from sync_repair.marker import mark_missing

    tiny = np.zeros((20, 30, 3), dtype=np.uint8)
    assert mark_missing(tiny).shape == tiny.shape


# ── Cameras that drop out or join late ───────────────────────────────────────


def test_a_camera_that_drops_out_does_not_cut_the_others():
    # The rig case: Camera 2 unplugged at tick 215 and never came back, while
    # five cameras ran to 558. The first version trimmed everyone to 215.
    ticks = _ticks(558)
    cams = [_camera(i, ticks, range(558)) for i in (0, 2, 3, 4, 5)]
    cams.append(_camera(1, ticks, range(215)))
    plan = build_tick_plan(ticks, cams)
    assert plan.total_ticks == 558
    dropped = plan.cameras[1]
    assert dropped.dropped_out_at == 215
    assert not dropped.missing[:215].any() and dropped.missing[215:].all()
    for i in (0, 2, 3, 4, 5):
        assert not plan.cameras[i].missing.any()
        assert plan.cameras[i].tail_trimmed == 0


def test_ordinary_start_and_stop_differences_are_still_trimmed():
    # 7 ticks late and 3 early at 25 fps: well inside the tolerance.
    ticks = _ticks(200)
    cams = [_camera(0, ticks, range(200)), _camera(1, ticks, range(7, 197))]
    plan = build_tick_plan(ticks, cams)
    assert plan.first_tick == 7 and plan.total_ticks == 190
    assert plan.cameras[1].dropped_out_at is None
    assert plan.cameras[1].joined_late_at is None
    assert not plan.cameras[1].missing.any()


def test_a_camera_that_joins_much_later_is_missing_until_it_arrives():
    ticks = _ticks(300)
    cams = [_camera(0, ticks, range(300)), _camera(1, ticks, range(120, 300))]
    plan = build_tick_plan(ticks, cams)
    assert plan.first_tick == 0 and plan.total_ticks == 300
    late = plan.cameras[1]
    assert late.joined_late_at == 120
    assert late.missing[:120].all() and not late.missing[120:].any()


# ── Cameras that drop out and come back ──────────────────────────────────────


def _reconnected_camera(index, tick_times, before, after, *, clock_restarts, jitter=None):
    """A camera that answered the ticks in `before`, dropped out, and answered
    the ticks in `after` once reconnected. frame_id keeps counting across the
    gap (the grabber reopens in place). With `clock_restarts`, its hardware
    clock starts again from near zero, as when the camera lost power."""
    a = _camera(index, tick_times, before, jitter=jitter)
    b = _camera(index, tick_times, after, jitter=jitter, seed=99)
    if clock_restarts:
        b.hw_ns = b.hw_ns - b.hw_ns[0] + 5_000_000  # 5 ms after boot
    return TickCamera(
        index=index,
        frame_ids=np.arange(1, len(before) + len(after) + 1, dtype=np.int64),
        elapsed_ns=np.concatenate((a.elapsed_ns, b.elapsed_ns)),
        hw_ns=np.concatenate((a.hw_ns, b.hw_ns)),
    )


def test_clock_segments_split_where_the_camera_clock_restarts_or_jumps():
    host = np.arange(10, dtype=np.int64) * PERIOD
    hw = host + 3_000_000_000
    assert clock_segments(hw, host).tolist() == [0] * 10
    restarted = hw.copy()
    restarted[6:] = host[6:] - host[6] + 1_000  # back to near zero
    assert clock_segments(restarted, host).tolist() == [0] * 6 + [1] * 4
    jumped = hw.copy()
    jumped[3:] += 60_000_000_000  # a minute ahead of the host
    assert clock_segments(jumped, host).tolist() == [0] * 3 + [1] * 7


def test_a_reconnected_camera_whose_clock_restarted_lands_on_the_right_ticks():
    # Unplugged at tick 150, back at tick 300, its clock restarted from zero.
    # One line through both stretches would place every frame after the gap
    # hundreds of ticks off (or before the start of the recording).
    ticks = _ticks(500)
    back = list(range(150)) + list(range(300, 500))
    cams = [_camera(0, ticks, range(500), jitter=3_000_000)]
    cams.append(
        _reconnected_camera(
            1, ticks, range(150), range(300, 500), clock_restarts=True, jitter=3_000_000
        )
    )
    plan = build_tick_plan(ticks, cams)
    assert plan.total_ticks == 500
    assert _real_ticks(plan, 1) == back
    assert gap_ranges(plan.cameras[1].missing) == [(150, 299)]
    assert plan.cameras[1].method == "trigger_ticks:hw_timestamp"
    assert not plan.cameras[1].uncertain
    assert not plan.cameras[0].missing.any()


def test_a_reconnected_camera_whose_clock_kept_running_needs_no_split():
    ticks = _ticks(500)
    cams = [
        _camera(0, ticks, range(500)),
        _reconnected_camera(1, ticks, range(150), range(300, 500), clock_restarts=False),
    ]
    plan = build_tick_plan(ticks, cams)
    assert gap_ranges(plan.cameras[1].missing) == [(150, 299)]
    assert _real_ticks(plan, 1) == list(range(150)) + list(range(300, 500))


def test_a_single_frame_between_two_clock_restarts_is_placed_by_arrival():
    # A camera that came back, delivered one frame, and dropped again: that
    # frame's stretch has no line of its own, and must still land correctly.
    ticks = _ticks(400)
    lone = _camera(1, ticks, [200], seed=7)
    lone.hw_ns = np.array([12_345], dtype=np.int64)
    a = _camera(1, ticks, range(100))
    b = _camera(1, ticks, range(300, 400), seed=3)
    b.hw_ns = b.hw_ns - b.hw_ns[0] + 1_000
    cam1 = TickCamera(
        index=1,
        frame_ids=np.arange(1, 202, dtype=np.int64),
        elapsed_ns=np.concatenate((a.elapsed_ns, lone.elapsed_ns, b.elapsed_ns)),
        hw_ns=np.concatenate((a.hw_ns, lone.hw_ns, b.hw_ns)),
    )
    plan = build_tick_plan(ticks, [_camera(0, ticks, range(400)), cam1])
    assert _real_ticks(plan, 1) == list(range(100)) + [200] + list(range(300, 400))


def test_stretches_with_a_latency_near_half_a_period_stay_on_one_grid():
    # The phase of each clock stretch is only known modulo a period. With a
    # latency right at half a period, a millisecond either way wraps one
    # stretch's phase to -P/2 and the other's to +P/2: without lining the
    # stretches up, everything after the reconnect sat one tick off.
    ticks = _ticks(400)
    half = PERIOD // 2
    a = _camera(1, ticks, range(150), latency=half + 1_000_000)
    b = _camera(1, ticks, range(250, 400), latency=half - 1_000_000)
    b.hw_ns = b.hw_ns - b.hw_ns[0] + 5_000_000
    cam1 = TickCamera(
        index=1,
        frame_ids=np.arange(1, 301, dtype=np.int64),
        elapsed_ns=np.concatenate((a.elapsed_ns, b.elapsed_ns)),
        hw_ns=np.concatenate((a.hw_ns, b.hw_ns)),
    )
    plan = build_tick_plan(ticks, [_camera(0, ticks, range(400), latency=half), cam1])
    assert _real_ticks(plan, 1) == list(range(150)) + list(range(250, 400))
