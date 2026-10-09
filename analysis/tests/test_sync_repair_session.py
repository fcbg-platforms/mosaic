"""
End to end: a small synthetic recording (real mp4s, timestamp CSVs, a trigger
tick log) through run_sync_repair.process_session(), checking the files a user
actually gets — equal frame counts, missing frames tagged in the image, and
the report.
"""

import csv
import json
import sys
from pathlib import Path

import cv2
import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_sync_repair  # noqa: E402

W, H = 160, 120
PERIOD = 40_000_000
LATENCY = 45_000_000


def _write_video(path: Path, n: int, shade: int) -> None:
    writer = None
    for fourcc in ("mp4v", "avc1"):
        writer = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*fourcc), 25.0, (W, H))
        if writer.isOpened():
            break
    assert writer is not None and writer.isOpened()
    for i in range(n):
        frame = np.full((H, W, 3), shade, dtype=np.uint8)
        cv2.putText(frame, str(i), (60, 70), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 255), 2)
        writer.write(frame)
    writer.release()


def _make_session(
    tmp_path: Path, answered: dict[int, list[int]], n_ticks: int, with_tick_log: bool = True
) -> Path:
    session = tmp_path / "sub-01_run-01"
    video = session / "video"
    video.mkdir(parents=True)
    ticks = [1_000_000_000 + k * PERIOD for k in range(n_ticks)]
    for cam, ks in answered.items():
        _write_video(video / f"video_{cam}.mp4", len(ks), shade=40 + 30 * cam)
        with (video / f"timestamps_cam{cam}.csv").open("w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["frame_id", "elapsed_ns", "wall_ns", "hw_timestamp_ns"])
            for j, k in enumerate(ks):
                w.writerow([j + 1, ticks[k] + LATENCY, 0, ticks[k] + 5_000_000_000 * (cam + 1)])
    if with_tick_log:
        with (video / "action_ticks.csv").open("w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["tick", "elapsed_ns", "fired"])
            for k, t in enumerate(ticks):
                w.writerow([k, t, len(answered)])
        (video / "action_group.json").write_text(json.dumps({"cameras": sorted(answered)}))
    (session / "session_meta.json").write_text(
        json.dumps({"cameras": [{"index": c} for c in sorted(answered)]})
    )
    return session


def _count_frames(path: Path) -> int:
    cap = cv2.VideoCapture(str(path))
    n = 0
    while cap.read()[0]:
        n += 1
    cap.release()
    return n


def _frame(path: Path, index: int) -> np.ndarray:
    cap = cv2.VideoCapture(str(path))
    frame = None
    for _ in range(index + 1):
        ok, frame = cap.read()
        assert ok
    cap.release()
    return frame


def test_trigger_aligned_session_has_equal_counts_and_tagged_gaps(tmp_path):
    answered = {
        0: list(range(60)),
        1: list(range(4, 60)),  # joined 4 ticks late
        2: [k for k in range(60) if k not in (20, 21, 22)],  # lost three frames
    }
    session = _make_session(tmp_path, answered, 60)
    run_sync_repair.process_session(session, 0.0)

    synced = session / "synced"
    report = json.loads((synced / "sync_repair.json").read_text())
    assert report["alignment"] == "trigger_ticks"
    assert report["first_tick"] == 4
    assert report["total_ticks"] == 56
    assert abs(report["tick_rate_fps"] - 25.0) < 1e-6

    counts = {c: _count_frames(synced / f"video_{c}.mp4") for c in answered}
    assert set(counts.values()) == {56}

    cams = {c["index"]: c for c in report["cameras"]}
    assert cams[0]["lead_in_trimmed"] == 4
    assert cams[2]["missing_frame_count"] == 3
    assert cams[2]["gaps"] == [[16, 18]]  # ticks 20-22, output starts at tick 4
    assert cams[0]["missing_frame_count"] == 0
    assert cams[2]["alignment"] == "trigger_ticks:hw_timestamp"

    # The missing frames carry the red tag; real ones do not.
    tagged = _frame(synced / "video_2.mp4", 17)
    clean = _frame(synced / "video_2.mp4", 10)
    assert tagged[1, 2, 2] > 150 and tagged[1, 2, 0] < 120  # red corner (BGR, lossy)
    assert not (clean[1, 2, 2] > 150 and clean[1, 2, 0] < 120)

    with (synced / "video_2.repair_map.csv").open() as f:
        rows = list(csv.DictReader(f))
    assert rows[16]["missing"] == "true" and rows[16]["tick"] == "20"
    assert rows[15]["missing"] == "false"


def test_session_without_a_tick_log_falls_back_to_arrival_time(tmp_path):
    answered = {0: list(range(40)), 1: list(range(40))}
    session = _make_session(tmp_path, answered, 40, with_tick_log=False)
    run_sync_repair.process_session(session, 0.0)
    report = json.loads((session / "synced" / "sync_repair.json").read_text())
    assert report["alignment"] == "arrival_time"
    counts = {_count_frames(session / "synced" / f"video_{c}.mp4") for c in answered}
    assert len(counts) == 1


@pytest.mark.parametrize("explicit_fps", [12.5])
def test_an_explicit_rate_overrides_the_ticks(tmp_path, explicit_fps):
    answered = {0: list(range(40)), 1: list(range(40))}
    session = _make_session(tmp_path, answered, 40)
    run_sync_repair.process_session(session, explicit_fps)
    report = json.loads((session / "synced" / "sync_repair.json").read_text())
    assert report["alignment"] == "arrival_time"
    assert report["master_fps"] == explicit_fps


def test_a_camera_with_an_empty_timestamps_file_is_skipped_not_fatal(tmp_path):
    # A camera that disconnected at the start of a triggered recording: its
    # CSV has only the header. That used to crash the whole repair.
    answered = {0: list(range(40)), 1: list(range(40)), 2: list(range(40))}
    session = _make_session(tmp_path, answered, 40)
    (session / "video" / "timestamps_cam2.csv").write_text(
        "frame_id,elapsed_ns,wall_ns,hw_timestamp_ns\n"
    )
    run_sync_repair.process_session(session, 0.0)
    report = json.loads((session / "synced" / "sync_repair.json").read_text())
    cams = {c["index"]: c for c in report["cameras"]}
    assert cams[2]["skipped"]
    assert not cams[0]["skipped"] and not cams[1]["skipped"]
    assert report["alignment"] == "trigger_ticks"


def test_a_cut_off_last_line_in_the_tick_log_is_ignored(tmp_path):
    # A crash, or a ticker killed mid-write, leaves "57,1234" where
    # "57,3280000000,3" was — numbers that still parse.
    answered = {0: list(range(60)), 1: list(range(60))}
    session = _make_session(tmp_path, answered, 60)
    log = session / "video" / "action_ticks.csv"
    lines = log.read_text().splitlines()
    log.write_text("\n".join(lines[:58] + ["57,1234"]) + "\n")
    ticks, cams = run_sync_repair._load_trigger_ticks(session)
    assert len(ticks) == 57
    assert (np.diff(ticks) > 0).all()
    assert cams == {0, 1}
