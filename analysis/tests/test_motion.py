"""
Motion tracking (motion/ and run_motion.py) on synthetic videos: a grey
arena and white squares moving at known speeds. Needs only OpenCV and numpy;
the plots also need matplotlib and are skipped without it.
"""

import argparse
import csv
import json
import sys
from pathlib import Path

import cv2
import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_motion
from motion.centroid_tracker import CentroidTracker, Track

FPS = 20.0
W, H = 320, 240
SIDE = 24  # square side, px: area 576


def write_video(path: Path, frames: int, squares) -> Path:
    """`squares(i)` gives the (x, y) top-left corners to draw in frame i."""
    out = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*"MJPG"), FPS, (W, H))
    assert out.isOpened()
    for i in range(frames):
        img = np.full((H, W, 3), 60, np.uint8)
        for x, y in squares(i):
            cv2.rectangle(
                img, (int(x), int(y)), (int(x) + SIDE, int(y) + SIDE), (255, 255, 255), -1
            )
        out.write(img)
    out.release()
    return path


def one_square(i):
    # Empty arena for 30 frames (the background model learns it), then a
    # square moving right at 3 px per frame.
    return [] if i < 30 else [(20 + 3 * (i - 30), 100)]


def tracker(**kw):
    kw.setdefault("min_area", 200)
    kw.setdefault("max_area", 2000)
    return CentroidTracker(**kw)


def test_one_moving_square_is_one_track_at_its_speed(tmp_path):
    video = write_video(tmp_path / "v.avi", 90, one_square)
    rows, traj, vels, size = run_motion.process_video(video, tracker(), None, 1, False)

    assert size == (W, H)
    assert {r["animal_id"] for r in rows} == {0}
    xs = [r["cx_px"] for r in rows]
    assert xs == sorted(xs)  # moving right
    # Steady state: 3 px/frame at 20 fps = 60 px/s (mm_per_px 1).
    steady = [r["velocity_mm_s"] for r in rows[5:-5]]
    assert np.median(steady) == pytest.approx(60.0, abs=6.0)
    # The centroid sits in the middle of the square.
    assert np.median([r["cy_px"] for r in rows]) == pytest.approx(100 + SIDE / 2, abs=1.5)


def test_timestamps_follow_the_video_without_a_timestamps_file(tmp_path):
    video = write_video(tmp_path / "v.avi", 60, one_square)
    rows, *_ = run_motion.process_video(video, tracker(), None, 1, False)
    for r in rows:
        assert r["timestamp_ns"] == round(r["frame"] * 1e9 / FPS)


def test_skip_does_not_inflate_the_speed(tmp_path):
    # Every 3rd frame analysed: positions are 9 px apart, still 60 px/s.
    video = write_video(tmp_path / "v.avi", 120, one_square)
    rows, *_ = run_motion.process_video(video, tracker(), None, 3, False)
    steady = [r["velocity_mm_s"] for r in rows[3:-3]]
    assert np.median(steady) == pytest.approx(60.0, abs=6.0)


def test_times_continue_past_a_short_timestamps_file(tmp_path):
    video = write_video(tmp_path / "v.avi", 60, one_square)
    ts = [5_000_000_000 + i * 50_000_000 for i in range(40)]  # 20 frames short
    rows, *_ = run_motion.process_video(video, tracker(), ts, 1, False)
    times = [r["timestamp_ns"] for r in rows]
    assert times == sorted(times)
    last = rows[-1]
    assert last["timestamp_ns"] == ts[-1] + round((last["frame"] - 39) * 1e9 / FPS)


def test_rows_stop_when_the_animal_is_gone(tmp_path):
    # The square is seen from frame 30 to 59, then gone: its track is kept
    # alive for max_lost frames, but no row may repeat its last position.
    video = write_video(
        tmp_path / "v.avi", 90, lambda i: [(40 + 3 * (i - 30), 80)] if 30 <= i < 60 else []
    )
    rows, *_ = run_motion.process_video(video, tracker(max_lost=20), None, 1, False)
    assert rows
    assert max(r["frame"] for r in rows) < 62
    frames = [r["frame"] for r in rows]
    assert len(frames) == len(set(frames))  # one row per frame for one animal


def test_two_animals_get_two_tracks_and_the_cap_keeps_one(tmp_path):
    def two(i):
        return [] if i < 30 else [(20 + 2 * (i - 30), 40), (260 - 2 * (i - 30), 170)]

    video = write_video(tmp_path / "v.avi", 80, two)
    rows, traj, *_ = run_motion.process_video(video, tracker(), None, 1, False)
    assert len(traj) == 2
    # Each track stays on its own row of the arena (no identity swap).
    for pts in traj.values():
        ys = [p[1] for p in pts]
        assert max(ys) - min(ys) < 5

    rows1, traj1, *_ = run_motion.process_video(video, tracker(n_animals=1), None, 1, False)
    assert len(traj1) == 1


def test_reset_keeps_the_trackers_own_background_settings():
    # run_motion resets the tracker before every video; reset() used to
    # rebuild the background model with fixed values (500, 16).
    t = CentroidTracker(history=120, var_threshold=40.0)
    t.reset()
    assert t._fgbg.getHistory() == 120
    assert t._fgbg.getVarThreshold() == pytest.approx(40.0)


def test_track_speed_from_its_last_two_positions():
    t = Track(id=0)
    assert t.velocity_px_per_frame == 0.0 and t.position == (0.0, 0.0)
    t.positions.extend([(0.0, 0.0), (3.0, 4.0)])
    t.areas.extend([100, 300])
    assert t.velocity_px_per_frame == pytest.approx(5.0)
    assert t.velocity_mm_per_s(mm_per_px=0.5, fps=30) == pytest.approx(75.0)
    assert t.mean_area == pytest.approx(200.0)


def test_load_timestamps_reads_elapsed_ns_not_the_frame_number(tmp_path):
    p = tmp_path / "timestamps_cam0.csv"
    p.write_text(
        "frame_id,elapsed_ns,wall_ns,hw_timestamp_ns\n1,5120000000,0,0\n2,5160000000,0,0\n"
    )
    assert run_motion.load_timestamps(str(p)) == [5120000000, 5160000000]
    # A file without the header: first column.
    q = tmp_path / "plain.csv"
    q.write_text("100\n200\nnot a number\n")
    assert run_motion.load_timestamps(str(q)) == [100, 200]
    assert run_motion.load_timestamps(str(tmp_path / "missing.csv")) is None
    assert run_motion.load_timestamps(None) is None


@pytest.fixture
def no_histogram(monkeypatch):
    """The speed histogram has no off switch and needs matplotlib, which
    CI's light test environment does not have; the plots have their own
    test."""
    monkeypatch.setattr(run_motion, "generate_velocity_histogram", lambda *a, **k: None)


def session_args(session: Path, *extra) -> argparse.Namespace:
    return run_motion.build_parser().parse_args(
        ["--session", str(session), "--min-area", "200", "--max-area", "2000", *extra]
    )


def make_session(tmp_path: Path, frames: int = 70) -> Path:
    session = tmp_path / "sub-01_ses-1_run-01"
    (session / "video").mkdir(parents=True)
    write_video(session / "video" / "video_0.avi", frames, one_square)
    with (session / "video" / "timestamps_cam0.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["frame_id", "elapsed_ns", "wall_ns", "hw_timestamp_ns"])
        for i in range(frames):
            w.writerow([i + 1, 7_000_000_000 + i * 50_000_000, 0, 0])
    return session


def test_session_mode_writes_rows_with_the_sessions_timestamps(tmp_path, no_histogram):
    session = make_session(tmp_path)
    run_motion.process_session(session_args(session, "--no-heatmap", "--no-trajectory"))

    with (session / "motion_results.csv").open() as f:
        rows = list(csv.DictReader(f))
    assert rows
    for r in rows:
        assert int(r["timestamp_ns"]) == 7_000_000_000 + int(r["frame"]) * 50_000_000


def test_session_mode_json(tmp_path, no_histogram):
    session = make_session(tmp_path)
    run_motion.process_session(
        session_args(session, "--out-format", "json", "--no-heatmap", "--no-trajectory")
    )
    rows = json.loads((session / "motion_results.json").read_text())
    assert rows and set(rows[0]) >= {"frame", "timestamp_ns", "animal_id", "cx_mm", "velocity_mm_s"}


def test_session_mode_plots(tmp_path):
    pytest.importorskip("matplotlib")
    session = make_session(tmp_path)
    run_motion.process_session(session_args(session))
    for name in (
        "motion_heatmap_cam0.png",
        "motion_trajectories_cam0.png",
        "motion_velocity_hist_cam0.png",
    ):
        png = session / name
        assert png.exists() and png.stat().st_size > 1000, name
        assert png.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"


def test_each_camera_keeps_its_own_animals_and_plots(tmp_path, monkeypatch):
    # Cameras 0 and 2 (camera 1 missing): both see "animal 0", which must
    # stay two animals, one per camera.
    session = make_session(tmp_path)
    write_video(
        session / "video" / "video_2.avi",
        70,
        lambda i: [] if i < 30 else [(250 - 3 * (i - 30), 40)],
    )
    plots = []
    monkeypatch.setattr(
        run_motion,
        "generate_heatmap",
        lambda traj, size, out, **k: plots.append((out, sorted(traj))),
    )
    monkeypatch.setattr(run_motion, "generate_trajectory_plot", lambda *a, **k: None)
    monkeypatch.setattr(run_motion, "generate_velocity_histogram", lambda *a, **k: None)

    run_motion.process_session(session_args(session))

    with (session / "motion_results.csv").open() as f:
        rows = list(csv.DictReader(f))
    by_cam = {}
    for r in rows:
        by_cam.setdefault(int(r["camera_index"]), set()).add(int(r["animal_id"]))
    assert by_cam == {0: {0}, 2: {0}}
    # Camera 2 has no timestamps file: video time, not camera 0's clock.
    assert all(int(r["timestamp_ns"]) < 7_000_000_000 for r in rows if r["camera_index"] == "2")
    assert [Path(out).name for out, _ in plots] == [
        "motion_heatmap_cam0.png",
        "motion_heatmap_cam2.png",
    ]


def test_the_step_after_a_gap_is_not_a_speed_spike(tmp_path):
    # Seen frames 30-49, hidden 50-59, back at 60 where it would have been:
    # 3 px per frame throughout, so the speed stays near 60 px/s.
    def blinking(i):
        if i < 30 or 50 <= i < 60:
            return []
        return [(20 + 3 * (i - 30), 100)]

    video = write_video(tmp_path / "v.avi", 80, blinking)
    rows, traj, *_ = run_motion.process_video(video, tracker(max_lost=20), None, 1, False)
    assert len(traj) == 1  # the same animal after the gap
    after = [r for r in rows if r["frame"] >= 60]
    assert after
    assert max(r["velocity_mm_s"] for r in after) < 100


def test_camera_index_from_the_file_name():
    assert run_motion.camera_index_from_filename(Path("video_2.avi")) == 2
    assert run_motion.camera_index_from_filename(Path("video_12.mp4")) == 12
    assert run_motion.camera_index_from_filename(Path("clip.avi")) is None
    assert run_motion.camera_index_from_filename(Path("video_0_old.mp4")) is None


def test_session_skips_files_that_are_not_camera_recordings(tmp_path, no_histogram):
    session = make_session(tmp_path)
    write_video(session / "video" / "video_0_old.avi", 70, one_square)
    write_video(session / "video" / "clip.avi", 70, one_square)
    run_motion.process_session(session_args(session, "--no-heatmap", "--no-trajectory"))
    with (session / "motion_results.csv").open() as f:
        rows = list(csv.DictReader(f))
    # Only video_0.avi: one animal, one row per frame it was seen.
    frames = [r["frame"] for r in rows]
    assert len(frames) == len(set(frames))


def test_single_video_mode_writes_next_to_the_video(tmp_path, no_histogram):
    video = write_video(tmp_path / "clip.avi", 70, one_square)
    args = run_motion.build_parser().parse_args(
        [
            "--video",
            str(video),
            "--min-area",
            "200",
            "--max-area",
            "2000",
            "--no-heatmap",
            "--no-trajectory",
        ]
    )
    run_motion.process_single(args)
    assert (tmp_path / "motion_clip.csv").exists()
