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

from sync_repair.tick_alignment import TickCamera, build_tick_plan, gap_ranges

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
