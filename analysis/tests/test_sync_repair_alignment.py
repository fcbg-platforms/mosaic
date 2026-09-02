"""
Pure-logic tests for sync_repair/alignment.py — the correctness core of the
Frame Sync Repair plugin. A bug here would silently misalign which source
frame each output tick duplicates, or corrupt the plugin's whole success
criterion (every camera ending up with the exact same output frame count).
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent.parent))

from sync_repair.alignment import (
    CameraFrames,
    build_tick_grid,
    compute_master_fps,
    compute_window,
)


def _cam(index, frame_ids, elapsed_ns):
    return CameraFrames(
        index=index,
        frame_ids=np.array(frame_ids, dtype=np.int64),
        elapsed_ns=np.array(elapsed_ns, dtype=np.int64),
    )


# ── compute_master_fps ───────────────────────────────────────────────────────


def test_compute_master_fps_picks_the_fastest_camera_not_average_or_slowest():
    # cam0: 10 frames over 1s -> ~9 fps ((10-1)*1e9/1e9)
    cam0 = _cam(0, list(range(10)), [i * 100_000_000 for i in range(10)])
    # cam1: 30 frames over 1s -> ~29 fps (the fastest)
    cam1 = _cam(1, list(range(30)), [round(i * 1e9 / 29) for i in range(30)])
    # cam2: 5 frames over 1s -> ~4 fps (the slowest)
    cam2 = _cam(2, list(range(5)), [i * 250_000_000 for i in range(5)])

    fps = compute_master_fps([cam0, cam1, cam2])
    assert 28.0 < fps < 30.0  # matches cam1, not an average of the three


def test_compute_master_fps_excludes_cameras_with_fewer_than_two_frames():
    single = _cam(0, [0], [0])
    normal = _cam(1, list(range(10)), [i * 40_000_000 for i in range(10)])  # 25 fps
    fps = compute_master_fps([single, normal])
    assert 24.0 < fps < 26.0


def test_compute_master_fps_is_zero_when_no_camera_qualifies():
    assert compute_master_fps([_cam(0, [0], [0]), _cam(1, [], [])]) == 0.0
    assert compute_master_fps([]) == 0.0


# ── compute_window ───────────────────────────────────────────────────────────


def test_compute_window_uses_the_overlap_when_cameras_overlap():
    # cam0: [0, 1000], cam1: [200, 900] -> overlap window is [200, 900]
    cam0 = _cam(0, [0, 1], [0, 1000])
    cam1 = _cam(1, [0, 1], [200, 900])
    t_origin, t_end = compute_window([cam0, cam1])
    assert t_origin == 200
    assert t_end == 900


def test_compute_window_falls_back_to_union_when_cameras_do_not_overlap():
    # cam0 finishes at 100 before cam1 even starts at 500 -> no overlap.
    cam0 = _cam(0, [0, 1], [0, 100])
    cam1 = _cam(1, [0, 1], [500, 1000])
    t_origin, t_end = compute_window([cam0, cam1])
    assert t_origin == 0
    assert t_end == 1000


# ── build_tick_grid ───────────────────────────────────────────────────────────


def test_build_tick_grid_perfectly_synced_cameras_have_zero_duplicates():
    step_ns = round(1e9 / 25.0)
    n = 20
    elapsed = [i * step_ns for i in range(n)]
    cam0 = _cam(0, list(range(n)), elapsed)
    cam1 = _cam(1, list(range(100, 100 + n)), elapsed)

    grid = build_tick_grid([cam0, cam1], 25.0)
    assert grid.total_ticks == n

    for cam_idx, offset in ((0, 0), (1, 100)):
        assigned = grid.frame_ids_by_camera[cam_idx]
        expected = np.arange(offset, offset + n)
        np.testing.assert_array_equal(assigned, expected)


def test_build_tick_grid_fills_a_gap_with_the_nearest_frame_hold_last():
    # master @ 30fps -> step_ns ~= 33.33ms. Camera has frames at 0ms,
    # 33ms, 200ms, 233ms — a real ~167ms gap between the 2nd and 3rd frame
    # (about 5 tick-widths). Every tick that falls inside that gap must
    # resolve to frame_id 1 (the nearest one before the gap) until a tick
    # crosses the gap's own halfway point, at which point it should flip
    # to frame_id 2 (nearest-neighbor, not "always hold the earlier one").
    master_fps = 30.0
    step_ns = round(1e9 / master_fps)
    frame_ids = [0, 1, 2, 3]
    elapsed_ns = [0, 33_000_000, 200_000_000, 233_000_000]
    cam = _cam(0, frame_ids, elapsed_ns)

    grid = build_tick_grid([cam], master_fps)
    assigned = grid.frame_ids_by_camera[0]

    # t_origin == 0 (single camera, its own first frame). Ticks land at
    # 0, step_ns, 2*step_ns, ... Verify against hand-computed nearest
    # neighbors rather than trusting the implementation's own logic.
    for tick, a in enumerate(assigned):
        t_ideal = tick * step_ns
        expected = min(range(len(elapsed_ns)), key=lambda i: abs(elapsed_ns[i] - t_ideal))
        assert (
            a == frame_ids[expected]
        ), f"tick {tick}: expected frame {frame_ids[expected]}, got {a}"

    # And explicitly: at least one tick duplicates frame_id 1 (holding the
    # nearest frame across part of the gap) before the grid ever reaches
    # frame_id 2 — this IS the gap-filling behavior under test.
    ones = list(assigned).count(1)
    assert ones > 1, "expected frame_id 1 to be held (duplicated) across part of the gap"


def test_build_tick_grid_assigned_frame_ids_are_non_decreasing_per_camera():
    # Required for run_sync_repair.py's single-sequential-forward-pass video
    # read to be correct — if this ever regressed to non-monotonic, that
    # video-reading strategy would silently read frames out of order.
    master_fps = 17.0  # deliberately not a "nice" divisor of anything below
    frame_ids = list(range(50))
    rng = np.random.default_rng(42)
    # Irregular but strictly increasing elapsed_ns, simulating jitter.
    gaps = rng.integers(20_000_000, 120_000_000, size=50)
    elapsed_ns = np.cumsum(gaps).tolist()
    cam = _cam(0, frame_ids, elapsed_ns)

    grid = build_tick_grid([cam], master_fps)
    assigned = grid.frame_ids_by_camera[0]
    assert all(assigned[i] <= assigned[i + 1] for i in range(len(assigned) - 1))


def test_build_tick_grid_single_frame_camera_assigns_it_to_every_tick():
    # nf == 1 must never crash the ptr+1 < nf advance check, and must
    # legitimately duplicate that one frame across the whole grid.
    other = _cam(0, list(range(10)), [i * 40_000_000 for i in range(10)])  # 25fps, 360ms span
    lone = _cam(1, [999], [100_000_000])

    grid = build_tick_grid([other, lone], 25.0)
    assigned = grid.frame_ids_by_camera[1]
    assert len(assigned) == grid.total_ticks
    assert all(v == 999 for v in assigned)


def test_build_tick_grid_explicit_master_fps_produces_expected_step_and_ticks():
    cam = _cam(0, [0, 1, 2], [0, 500_000_000, 1_000_000_000])
    grid = build_tick_grid([cam], 10.0)
    assert grid.step_ns == 100_000_000  # 1e9 / 10.0
    # window is [0, 1e9], step 1e8 -> (1e9-0)//1e8 + 1 = 11 ticks
    assert grid.total_ticks == 11
    assert grid.master_fps == 10.0


def test_build_tick_grid_rejects_non_positive_master_fps():
    cam = _cam(0, [0, 1], [0, 40_000_000])
    for bad_fps in (0.0, -5.0):
        try:
            build_tick_grid([cam], bad_fps)
        except ValueError:
            continue
        raise AssertionError(f"expected ValueError for master_fps={bad_fps}")
