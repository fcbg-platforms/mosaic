"""
End to end: a small synthetic recording (real mp4s, timestamp CSVs, a trigger
tick log) through run_sync_repair.process_session(), checking the files a user
actually gets: repair maps of equal length, with the missing frames marked
and each frame's place in its video, and the report. The synced videos are
exported from these maps by MOSAIC (src/tools/export_synced_main.cpp).
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
    tmp_path: Path,
    answered: dict[int, list[int]],
    n_ticks: int,
    with_tick_log: bool = True,
    exposure_us: dict[int, float] | None = None,
) -> Path:
    """A recorded session. With `exposure_us`, every camera's timestamps file
    has the exposure column, and each frame arrives that much later."""
    session = tmp_path / "sub-01_run-01"
    video = session / "video"
    video.mkdir(parents=True)
    ticks = [1_000_000_000 + k * PERIOD for k in range(n_ticks)]
    for cam, ks in answered.items():
        _write_video(video / f"video_{cam}.mp4", len(ks), shade=40 + 30 * cam)
        with (video / f"timestamps_cam{cam}.csv").open("w", newline="") as f:
            w = csv.writer(f)
            header = ["frame_id", "elapsed_ns", "wall_ns", "hw_timestamp_ns"]
            w.writerow(header + (["exposure_us"] if exposure_us else []))
            for j, k in enumerate(ks):
                hw = ticks[k] + 5_000_000_000 * (cam + 1)
                if exposure_us:
                    exp = exposure_us[cam]
                    w.writerow([j + 1, ticks[k] + LATENCY + int(exp * 1000), 0, hw, exp])
                else:
                    w.writerow([j + 1, ticks[k] + LATENCY, 0, hw])
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


def _map(session: Path, cam: int) -> list[dict]:
    with (session / "synced" / f"video_{cam}.repair_map.csv").open() as f:
        return list(csv.DictReader(f))


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
    assert report["exposure_corrected"] is False  # files without exposure_us
    assert report["first_tick"] == 4
    assert report["total_ticks"] == 56
    assert abs(report["tick_rate_fps"] - 25.0) < 1e-6

    counts = {c: len(_map(session, c)) for c in answered}
    assert set(counts.values()) == {56}
    # No video is written: MOSAIC exports them on request, from the maps.
    assert not list(synced.glob("*.mp4"))

    cams = {c["index"]: c for c in report["cameras"]}
    assert cams[0]["lead_in_trimmed"] == 4
    assert cams[2]["missing_frame_count"] == 3
    assert cams[2]["gaps"] == [[16, 18]]  # ticks 20-22, output starts at tick 4
    assert cams[0]["missing_frame_count"] == 0
    assert cams[2]["alignment"] == "trigger_ticks:hw_timestamp"

    rows = _map(session, 2)
    assert rows[16]["missing"] == "true" and rows[16]["tick"] == "20"
    assert rows[15]["missing"] == "false"
    # Each frame's place in the video: tick 19 is camera 2's 20th frame (index 19),
    # the missing ticks 20-22 repeat it; tick 23 is its 21st.
    assert [int(rows[k]["source_frame_index"]) for k in (15, 16, 18, 19)] == [19, 19, 19, 20]
    # Camera 1 joined 4 ticks late: its first frame is the output's first.
    assert _map(session, 1)[0]["source_frame_index"] == "0"


def test_per_frame_exposure_keeps_cameras_on_the_same_tick(tmp_path):
    # Two cameras on the same trigger, one exposing 30 ms longer (auto
    # exposure in a darker corner): its frames arrive 30 ms later. At 40 ms
    # per tick that reads as a different tick unless the exposure is taken
    # off, which put that camera one frame off the other.
    answered = {0: list(range(60)), 1: list(range(60))}
    session = _make_session(tmp_path, answered, 60, exposure_us={0: 2000.0, 1: 32000.0})
    run_sync_repair.process_session(session, 0.0)

    report = json.loads((session / "synced" / "sync_repair.json").read_text())
    assert report["exposure_corrected"] is True
    assert report["total_ticks"] == 60
    for cam in report["cameras"]:
        assert cam["missing_frame_count"] == 0
        assert cam["lead_in_trimmed"] == 0 and cam["tail_trimmed"] == 0
        assert not cam["alignment_uncertain"]


def test_session_without_a_tick_log_falls_back_to_arrival_time(tmp_path):
    answered = {0: list(range(40)), 1: list(range(40))}
    session = _make_session(tmp_path, answered, 40, with_tick_log=False)
    run_sync_repair.process_session(session, 0.0)
    report = json.loads((session / "synced" / "sync_repair.json").read_text())
    assert report["alignment"] == "arrival_time"
    counts = {len(_map(session, c)) for c in answered}
    assert len(counts) == 1


def test_a_video_shorter_than_its_timestamps_repeats_its_last_frame(tmp_path):
    # A cut-off file: its timestamps list 60 frames, the video holds 50.
    answered = {0: list(range(60)), 1: list(range(60))}
    session = _make_session(tmp_path, answered, 60)
    _write_video(session / "video" / "video_1.mp4", 50, shade=70)
    run_sync_repair.process_session(session, 0.0)
    rows = _map(session, 1)
    assert [r["missing"] for r in rows[48:52]] == ["false", "false", "true", "true"]
    assert {r["source_frame_index"] for r in rows[49:]} == {"49"}
    report = json.loads((session / "synced" / "sync_repair.json").read_text())
    cam1 = next(c for c in report["cameras"] if c["index"] == 1)
    # Repeated, not missing from the camera: counted as duplicated, as before.
    assert "ended early" in cam1["note"] and cam1["duplicated_frame_count"] == 10


def test_synced_videos_of_an_earlier_run_are_removed(tmp_path):
    # They follow the earlier maps, not the new ones.
    answered = {0: list(range(40)), 1: list(range(40))}
    session = _make_session(tmp_path, answered, 40)
    (session / "synced").mkdir()
    (session / "synced" / "video_0.mp4").write_bytes(b"old")
    run_sync_repair.process_session(session, 0.0)
    assert not (session / "synced" / "video_0.mp4").exists()
    assert (session / "synced" / "video_0.repair_map.csv").exists()


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
