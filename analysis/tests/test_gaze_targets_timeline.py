"""
Tests for gaze/targets.py, gaze/filters.py and gaze/timeline.py.
"""

import csv
import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from gaze.filters import one_euro, smooth_directions, smooth_series
from gaze.ray_math import unit
from gaze.targets import FACE_MIN_CONE_DEG, Region, apply_min_dwell, cast_gaze
from gaze.timeline import load_raw, load_synced, load_timeline

# ── Targets ──────────────────────────────────────────────────────────────────


def test_looking_near_a_face_counts_within_the_cone():
    faces = {"S2": [0.0, 0.0, 2000.0]}
    hit = cast_gaze([0, 0, 0], unit([0.05, 0.0, 1.0]), faces=faces)  # ~2.9 deg off
    assert hit.kind == "subject" and hit.subject == "S2"
    assert hit.angle_off_deg == pytest.approx(2.86, abs=0.05)
    miss = cast_gaze([0, 0, 0], unit([0.25, 0.0, 1.0]), faces=faces)  # ~14 deg off
    assert miss.kind == "none"
    assert miss.point == pytest.approx(1500.0 * unit([0.25, 0.0, 1.0]))


def test_a_close_face_gets_its_full_angular_size():
    # At 500 mm a 110 mm head subtends ~12.7 degrees, more than the minimum.
    hit = cast_gaze([0, 0, 0], unit([0.2, 0.0, 1.0]), faces={"S2": [0, 0, 500.0]})
    assert hit.kind == "subject" and hit.angle_off_deg > FACE_MIN_CONE_DEG


def test_a_face_behind_the_subject_is_never_looked_at():
    assert cast_gaze([0, 0, 0], [0, 0, 1], faces={"S2": [0, 0, -1000.0]}).kind == "none"


def test_nearest_target_along_the_ray_wins():
    screen = Region("screen", [0, 0, 1000.0], [0, 0, -1], [1, 0, 0], 800.0, 500.0)
    plane = (np.array([0, 0, 3000.0]), np.array([0, 0, 1.0]))
    hit = cast_gaze(
        [0, 0, 0], [0, 0, 1], faces={"S2": [0, 0, 2000.0]}, regions=[screen], plane=plane
    )
    assert hit.kind == "region" and hit.label == "screen"
    assert hit.point == pytest.approx([0, 0, 1000.0])
    # Looking past the screen's edge: the face behind it is next.
    # (x = 450 mm where it crosses the screen's plane, the screen ends at 400.)
    hit = cast_gaze(
        [0, 0, 0],
        unit([0.45, 0, 1]),
        faces={"S2": [900.0, 0, 2000.0]},
        regions=[screen],
        plane=plane,
    )
    assert hit.kind == "subject"


def test_region_bounds_follow_its_own_axes():
    tilted = Region("poster", [0, 0, 1000.0], [0, 0, -1], [1, 1, 0], 400.0, 100.0)
    assert np.dot(tilted.u_axis, tilted.normal) == pytest.approx(0.0)
    # 141 mm along the 400 mm width hits; 141 mm across the 100 mm height misses.
    along = cast_gaze([0, 0, 0], unit([100.0, 100.0, 1000.0]), regions=[tilted])
    across = cast_gaze([0, 0, 0], unit([100.0, -100.0, 1000.0]), regions=[tilted])
    assert along.kind == "region" and across.kind == "none"
    assert tilted.corners().shape == (4, 3)


def test_short_label_changes_are_suppressed_but_real_ones_kept():
    t = np.arange(20) / 20.0  # 50 ms ticks
    labels = ["S2"] * 8 + ["none"] * 2 + ["S2"] * 4 + ["screen"] * 6
    out = apply_min_dwell(labels, t, min_dwell_s=0.15)
    assert out[8:10] == ["S2", "S2"]  # a 100 ms blip is ignored
    assert out[14:] == ["screen"] * 6  # 300 ms is a real change


def test_missing_estimates_do_not_count_as_changes():
    t = np.arange(6) / 10.0
    assert apply_min_dwell(["a", None, None, "a", "b", "b"], t, 0.15) == [
        "a",
        None,
        None,
        "a",
        "b",
        "b",
    ]


# ── Smoothing ────────────────────────────────────────────────────────────────


def test_one_euro_removes_jitter_on_a_still_signal():
    rng = np.random.default_rng(0)
    t = np.arange(200) / 25.0
    x = 10.0 + rng.normal(0, 1.0, 200)
    y = one_euro(x, t, min_cutoff_hz=0.5, beta=0.0)
    assert np.std(y[50:]) < 0.35 * np.std(x[50:])


def test_forward_backward_smoothing_has_no_lag_on_a_step():
    t = np.arange(100) / 25.0
    x = np.where(np.arange(100) < 50, 0.0, 10.0)
    y = smooth_series(x, t, np.ones(100, bool), min_cutoff_hz=1.0, beta=0.5)
    assert abs(np.argmin(np.abs(y - 5.0)) - 49.5) <= 1.5  # crosses the midpoint at the step


def test_gaps_are_not_bridged():
    t = np.arange(20) / 25.0
    t[10:] += 5.0  # a 5 s gap
    x = np.where(np.arange(20) < 10, 0.0, 100.0)
    y = smooth_series(x, t, np.ones(20, bool))
    assert np.allclose(y[:10], 0.0) and np.allclose(y[10:], 100.0)


def test_smoothed_directions_stay_unit():
    d = np.tile([0.0, 0.0, 1.0], (30, 1)) + np.random.default_rng(1).normal(0, 0.02, (30, 3))
    out = smooth_directions(d, np.arange(30) / 25.0, np.ones(30, bool))
    assert np.allclose(np.linalg.norm(out, axis=1), 1.0)


# ── Timeline ─────────────────────────────────────────────────────────────────


def _write_csv(path, header, rows):
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def _synced_session(tmp_path, trigger: bool):
    s = tmp_path / "sess"
    (s / "synced").mkdir(parents=True)
    (s / "video").mkdir()
    total = 5
    for cam in (0, 1):
        (s / "synced" / f"video_{cam}.mp4").write_bytes(b"x")
        rows = [
            [
                k,
                10 + k,
                "true" if (cam == 1 and k == 2) else "false",
                "true" if (cam == 1 and k == 2) else "false",
                (100 + k) if trigger else "",
            ]
            for k in range(total)
        ]
        _write_csv(
            s / "synced" / f"video_{cam}.repair_map.csv",
            ["output_frame_index", "source_frame_id", "duplicated", "missing", "tick"],
            rows,
        )
        _write_csv(
            s / "video" / f"timestamps_cam{cam}.csv",
            ["frame_id", "elapsed_ns", "wall_ns", "hw_timestamp_ns"],
            [[10 + k, 1_000_000_000 + k * 40_000_000 + cam, 0, 0] for k in range(total)],
        )
    report = {
        "alignment": "trigger_ticks" if trigger else "arrival_time",
        "master_fps": 25.0,
        "first_tick": 100 if trigger else None,
        "total_ticks": total,
        "cameras": [
            {"index": 0, "repaired_video": "synced/video_0.mp4", "skipped": False},
            {"index": 1, "repaired_video": "synced/video_1.mp4", "skipped": False},
            {"index": 2, "repaired_video": None, "skipped": True},
        ],
    }
    (s / "synced" / "sync_repair.json").write_text(json.dumps(report))
    if trigger:
        _write_csv(
            s / "video" / "action_ticks.csv",
            ["tick", "elapsed_ns", "fired"],
            [[k, 5_000 + k * 40_000_000, 2] for k in range(110)],
        )
    return s


def test_synced_trigger_mode_uses_the_tick_log(tmp_path):
    tl = load_synced(_synced_session(tmp_path, trigger=True))
    assert tl.source == "synced" and tl.n_ticks == 5 and tl.first_tick == 100
    assert sorted(tl.cameras) == [0, 1]  # the skipped camera is left out
    assert tl.times_ns[0] == 5_000 + 100 * 40_000_000
    assert list(tl.cameras[1].missing) == [False, False, True, False, False]
    assert list(tl.cameras[0].frame_index) == [0, 1, 2, 3, 4]


def test_synced_arrival_mode_uses_the_source_frames_times(tmp_path):
    tl = load_synced(_synced_session(tmp_path, trigger=False))
    assert tl.first_tick is None
    # Median of the two cameras' frame times (they differ by 1 ns).
    assert tl.times_ns[0] in (1_000_000_000, 1_000_000_001)
    assert np.all(np.diff(tl.times_ns) > 0)


def test_raw_mode_marks_frames_far_from_their_tick_missing(tmp_path):
    s = tmp_path / "raw"
    (s / "video").mkdir(parents=True)
    (s / "video" / "video_0.mp4").write_bytes(b"x")
    # Frames at 0, 40, 80 ms and then a late one at 200 ms.
    _write_csv(
        s / "video" / "timestamps_cam0.csv",
        ["frame_id", "elapsed_ns"],
        [[1, 0], [2, 40_000_000], [3, 80_000_000], [4, 200_000_000]],
    )
    manifest = {
        "t_origin_ns": "0",
        "step_ns": "40000000",
        "master_fps": 25.0,
        "total_ticks": 4,
        "cameras": [{"index": 0, "video_file": "video/video_0.mp4"}],
        "ticks": {"cam0_frame_ids": [1, 2, 3, 4]},
    }
    (s / "sync_manifest.json").write_text(json.dumps(manifest))
    tl = load_raw(s)
    assert list(tl.cameras[0].frame_index) == [0, 1, 2, 3]
    assert list(tl.cameras[0].missing) == [False, False, False, True]  # 200 ms vs tick at 120 ms
    assert load_timeline(s).source == "raw"  # no synced/ -> raw


def test_no_sync_information_gives_no_timeline(tmp_path):
    assert load_timeline(tmp_path) is None


def test_a_blip_before_a_gap_is_suppressed_and_never_crosses_it():
    # A one-sample B right before a 5 s gap: not credited with the gap's length.
    assert apply_min_dwell(
        ["A", "A", "A", "B", None, "A"], [0, 0.033, 0.066, 0.1, 5, 10], 0.15
    ) == [
        "A",
        "A",
        "A",
        "A",
        None,
        "A",
    ]
    # And a label never carries over a gap onto the next stretch.
    assert (
        apply_min_dwell(["A", "A", "B", "A", "A"], [0, 0.033, 0.066, 10, 10.03], 0.15) == ["A"] * 5
    )


def test_a_short_run_at_the_start_takes_the_next_label():
    t = [i * 0.05 for i in range(7)]
    assert apply_min_dwell(["B", "A", "A", "A", "A", "A", "A"], t, 0.15) == ["A"] * 7


# ── Subjects over time ───────────────────────────────────────────────────────


def _entry(tick, x):
    from gaze.fusion import Entry
    from gaze.head_pose import HeadPose

    return Entry(
        tick=tick, views=[], pose=HeadPose(np.eye(3), np.array([x, 0.0, 2500.0]), 1.0, 0.0)
    )


def test_someone_who_walks_away_and_back_stays_one_subject():
    from gaze.fusion import assign_subjects

    times = np.arange(400) / 25.0
    # Seated at x=0 for 4 s, unseen 3 s, standing 1.5 m away for 4 s.
    entries = [_entry(k, 0.0) for k in range(0, 100)] + [_entry(k, 1500.0) for k in range(175, 275)]
    assert assign_subjects(entries, times) == ["S1"]
    assert {e.subject for e in entries} == {"S1"}


def test_an_impossible_jump_is_a_second_person():
    from gaze.fusion import assign_subjects

    times = np.arange(400) / 25.0
    # 4 m away 0.2 s later: nobody walks that fast.
    entries = [_entry(k, 0.0) for k in range(0, 100)] + [_entry(k, 4000.0) for k in range(105, 205)]
    assert assign_subjects(entries, times) == ["S1", "S2"]
