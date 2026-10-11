"""
run_pose.py around the model: which files it reads, what it writes and where,
with a stand-in estimator (no YOLO or torch needed). The model itself is
covered by tests/test_human_pose_subject_ids.py.
"""

import csv
import json
import sys
from pathlib import Path

import cv2
import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_pose
from pose.keypoints import COCO_KEYPOINTS, PoseResult, SubjectPose

FPS = 10.0
SIZE = (160, 120)


def write_mp4(path: Path, frames: int) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    out = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*"mp4v"), FPS, SIZE)
    assert out.isOpened()
    for i in range(frames):
        out.write(np.full((SIZE[1], SIZE[0], 3), i % 255, np.uint8))
    out.release()
    return path


def write_timestamps(path: Path, frames: int, start_ns: int) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["frame_id", "elapsed_ns", "wall_ns", "hw_timestamp_ns"])
        for i in range(frames):
            w.writerow([i + 1, start_ns + i * 100_000_000, 0, 0])


class FakeEstimator:
    """One subject per frame, at a position that encodes the frame index."""

    def __init__(self):
        self.resets = 0
        self.calls = []

    def reset_tracker(self):
        self.resets += 1

    def infer(self, frame, frame_index, timestamp_ns, camera_index):
        self.calls.append((frame_index, timestamp_ns, camera_index))
        kp = [(float(frame_index), float(k)) for k in range(len(COCO_KEYPOINTS))]
        subject = SubjectPose(
            subject_id=7,
            confidence=0.9,
            keypoints=kp,
            visibilities=[0.8] * len(COCO_KEYPOINTS),
            bbox_xyxy=(1.0, 2.0, 3.0, 4.0),
        )
        return PoseResult(
            frame_index=frame_index,
            timestamp_ns=timestamp_ns,
            camera_index=camera_index,
            subjects=[subject],
            backend="fake",
            inference_ms=1.234,
        )


def test_camera_index_comes_from_the_file_name():
    assert run_pose._camera_index_from_filename(Path("video_2.mp4")) == 2
    assert run_pose._camera_index_from_filename(Path("video_11.mp4")) == 11
    assert run_pose._camera_index_from_filename(Path("clip.mp4")) == 0


def test_session_writes_one_pose_file_per_camera_with_its_index_and_times(tmp_path):
    # Camera 1 missing (it failed to open): the files keep their own indices.
    session = tmp_path / "session"
    for cam, start in ((0, 1_000_000_000), (2, 5_000_000_000)):
        write_mp4(session / "video" / f"video_{cam}.mp4", 6)
        write_timestamps(session / "video" / f"timestamps_cam{cam}.csv", 6, start)
    est = FakeEstimator()

    run_pose.process_session(session, est, "yolov8n-pose.pt")

    assert est.resets == 2  # one identity space per video
    for cam, start in ((0, 1_000_000_000), (2, 5_000_000_000)):
        doc = json.loads((session / "pose" / f"video_{cam}.yolov8n-pose.pose.json").read_text())
        assert doc["model"] == "yolov8n-pose.pt"
        assert doc["keypoint_names"] == COCO_KEYPOINTS
        assert doc["tracker"] == run_pose.TRACKER_NAME
        frames = doc["frames"]
        assert len(frames) == 6
        assert [f["camera_index"] for f in frames] == [cam] * 6
        assert [f["timestamp_ns"] for f in frames] == [start + i * 100_000_000 for i in range(6)]
        s = frames[3]["subjects"][0]
        assert s["subject_id"] == 7 and s["keypoints"][0] == [3.0, 0.0]
        assert frames[0]["inference_ms"] == 1.23


def test_skip_runs_the_model_on_every_nth_frame_only(tmp_path):
    video = write_mp4(tmp_path / "video_0.mp4", 10)
    est = FakeEstimator()
    run_pose.process_video(video, est, "m.pt", skip=3)
    assert [c[0] for c in est.calls] == [0, 3, 6, 9]
    doc = json.loads((tmp_path / "video_0.m.pose.json").read_text())
    assert [f["frame_index"] for f in doc["frames"]] == [0, 3, 6, 9]
    # No timestamps file: zero, never a made-up time.
    assert {f["timestamp_ns"] for f in doc["frames"]} == {0}


def test_two_models_do_not_overwrite_each_other(tmp_path):
    video = write_mp4(tmp_path / "video_0.mp4", 3)
    run_pose.process_video(video, FakeEstimator(), "yolov8n-pose.pt")
    run_pose.process_video(video, FakeEstimator(), "yolo26s-pose.pt")
    assert (tmp_path / "video_0.yolov8n-pose.pose.json").exists()
    assert (tmp_path / "video_0.yolo26s-pose.pose.json").exists()


def test_csv_output_has_one_row_per_subject_and_frame(tmp_path):
    video = write_mp4(tmp_path / "video_0.mp4", 4)
    run_pose.process_video(video, FakeEstimator(), "m.pt", out_format="csv")
    with (tmp_path / "video_0.m.pose.csv").open() as f:
        rows = list(csv.reader(f))
    header = rows[0]
    assert header[:4] == ["frame", "timestamp_ns", "subject_id", "confidence"]
    assert len(header) == 4 + 3 * len(COCO_KEYPOINTS)
    assert header[4:7] == [
        f"{COCO_KEYPOINTS[0]}_x",
        f"{COCO_KEYPOINTS[0]}_y",
        f"{COCO_KEYPOINTS[0]}_vis",
    ]
    assert len(rows) == 1 + 4
    assert rows[2][:3] == ["1", "0", "7"]


def test_session_without_videos_exits(tmp_path):
    (tmp_path / "video").mkdir()
    with pytest.raises(SystemExit):
        run_pose.process_session(tmp_path, FakeEstimator(), "m.pt")


class FakeDepth:
    def __init__(self, fail_on=()):
        self.fail_on = set(fail_on)
        self.n = 0

    def infer_colorized(self, frame):
        i = self.n
        self.n += 1
        if i in self.fail_on:
            raise RuntimeError("model hiccup")
        return np.full_like(frame, 200)


def test_depth_writes_a_video_of_the_same_length_and_survives_a_failed_frame(tmp_path):
    session = tmp_path / "session"
    write_mp4(session / "video" / "video_1.mp4", 8)
    run_pose.process_session_depth(session, FakeDepth(fail_on={2}), "yolo26n-depth.pt")

    out = session / "depth" / "video_1.yolo26n-depth.mp4"
    assert out.exists()
    cap = cv2.VideoCapture(str(out))
    n = 0
    while cap.read()[0]:
        n += 1
    cap.release()
    assert n == 8
