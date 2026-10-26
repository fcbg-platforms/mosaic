"""Hands (hands/, run_hands.py) without the models: whose hand and which side,
repeats, when each hand is seen, and the runner with a stand-in model."""

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_hands
from hands.models import RTMW_HANDS, RTMPose, RtmwHands
from hands.people import (
    assign_by_wrist,
    box_overlap,
    distinct_people,
    drop_repeats,
    hand_seeds,
    hand_size,
    seen_spans,
    summarise,
)


def _person(pid, left=None, right=None, elbows=True):
    """A Pose person with the given wrists seen (COCO 9 left, 10 right) and,
    when ``elbows``, each elbow 100 px below its wrist."""
    kp = np.zeros((17, 2))
    vis = np.zeros(17)
    for wrist, elbow, at in ((9, 7, left), (10, 8, right)):
        if at is not None:
            kp[wrist] = at
            vis[wrist] = 1.0
            if elbows:
                kp[elbow] = (at[0], at[1] + 100)
                vis[elbow] = 1.0
    return {"subject_id": pid, "keypoints": kp.tolist(), "visibilities": vis.tolist()}


def _hand(wrist, size=80.0, score=0.9):
    """21 points: the wrist, the others spread over a ``size`` box above it."""
    rng = np.random.default_rng(0)
    pts = np.asarray(wrist, float) + rng.uniform([-size / 2, -size], [size / 2, 0], (21, 2))
    pts[0] = wrist
    pts[1] = (wrist[0] - size / 2, wrist[1] - size)  # the box's corners
    pts[2] = (wrist[0] + size / 2, wrist[1])
    return {"score": score, "keypoints": pts.tolist(), "scores": [score] * 21}


def test_hand_size_and_seeds_past_the_wrist():
    assert hand_size(_hand((100, 100), size=80)["keypoints"]) == pytest.approx(80)
    (c, size), *_ = hand_seeds([_person(1, left=(200, 200))])
    # Elbow 100 px below the wrist: 35 px further up, forearm-sized.
    assert c == pytest.approx((200, 165)) and size == pytest.approx(120)
    (_c, size), *_ = hand_seeds([_person(1, right=(50, 50), elbows=False)])
    assert size == 200.0
    assert hand_seeds([_person(1)]) == []


def test_assign_by_wrist_nearest_first_each_wrist_once():
    people = [_person(1, left=(100, 300), right=(400, 300)), _person(2, left=(700, 300))]
    hands = [_hand((405, 305)), _hand((110, 300)), _hand((1000, 300))]
    assign_by_wrist(hands, people)
    assert [(h["subject_id"], h["side"]) for h in hands] == [
        (1, "right"),
        (1, "left"),
        (None, "unknown"),  # no wrist within its size
    ]
    # Two hands at one wrist: the nearer gets it.
    two = assign_by_wrist([_hand((130, 300)), _hand((105, 300))], [_person(1, left=(100, 300))])
    assert [(h["subject_id"], h["side"]) for h in two] == [(None, "unknown"), (1, "left")]


def test_drop_repeats_keeps_the_better_score():
    a, b, c = _hand((100, 100), score=0.5), _hand((104, 102), score=0.8), _hand((400, 100))
    kept = drop_repeats([a, b, c])
    assert len(kept) == 2 and b in kept and c in kept
    # A person's two hands together (both on one ball) are both kept; two of
    # one side are one.
    left, right = dict(a, side="left"), dict(b, side="right")
    assert len(drop_repeats([left, right])) == 2
    assert drop_repeats([left, dict(b, side="left")]) == [dict(b, side="left")]
    # Two people's right hands meeting (a handshake) are both kept.
    mine, yours = dict(a, side="right", subject_id=1), dict(b, side="right", subject_id=2)
    assert len(drop_repeats([mine, yours])) == 2


def test_distinct_people_one_box_per_person():
    a = {"subject_id": 1, "confidence": 0.9, "bbox_xyxy": [0, 0, 100, 200]}
    twice = {"subject_id": 2, "confidence": 0.5, "bbox_xyxy": [5, 5, 100, 205]}
    beside = {"subject_id": 3, "confidence": 0.7, "bbox_xyxy": [80, 0, 180, 200]}
    assert box_overlap(a["bbox_xyxy"], a["bbox_xyxy"]) == 1.0
    assert box_overlap([0, 0, 1, 1], [2, 2, 3, 3]) == 0.0
    assert distinct_people([twice, a, beside, {"subject_id": 4}]) == [a, beside]


class _FakeSession:
    """An ONNX session whose SimCC output peaks where ``peak`` (input
    pixels, x and y) says, for every point."""

    def __init__(self, size, points, peak):
        self.size, self.points, self.peak = size, points, peak

    def get_inputs(self):
        return [type("I", (), {"name": "input"})()]

    def run(self, _outputs, feeds):
        (x,) = feeds.values()
        assert x.shape == (1, 3, self.size[1], self.size[0])
        sx = np.zeros((1, self.points, 2 * self.size[0]), np.float32)
        sy = np.zeros((1, self.points, 2 * self.size[1]), np.float32)
        sx[0, :, int(2 * self.peak[0])] = 0.8
        sy[0, :, int(2 * self.peak[1])] = 0.6
        return [sx, sy]


def _rtmpose(size, points, peak):
    m = RTMPose.__new__(RTMPose)
    m.session, m.input, m.size = _FakeSession(size, points, peak), "input", size
    return m


@pytest.mark.parametrize("box", [[100, 200, 300, 600], [50, 50, 450, 150], [10, 10, 30, 30]])
def test_rtmpose_maps_input_points_back_to_the_image(box):
    pytest.importorskip("cv2")
    rgb = np.zeros((720, 1280, 3), np.uint8)
    # The input's centre is the box's centre.
    xy, scores = _rtmpose((288, 384), 3, (144, 192))(rgb, box)
    assert xy[0] == pytest.approx(((box[0] + box[2]) / 2, (box[1] + box[3]) / 2), abs=1e-6)
    assert scores == pytest.approx([0.7] * 3)
    # The input's left edge is the widened box's: 1.25 times, then to 3:4.
    xy, _ = _rtmpose((288, 384), 1, (0, 192))(rgb, box)
    w, h = (box[2] - box[0]) * 1.25, (box[3] - box[1]) * 1.25
    half = max(w, h * 288 / 384) / 2
    assert xy[0][0] == pytest.approx((box[0] + box[2]) / 2 - half)


def _wholebody(left_at, right_at, score=0.9):
    """RTMW's 133 points: each hand's 21 around its place, 40 px across."""

    def body(_rgb, _box):
        xy = np.zeros((133, 2))
        sc = np.full(133, score)
        for first, at in ((RTMW_HANDS["left"], left_at), (RTMW_HANDS["right"], right_at)):
            xy[first : first + 21] = np.asarray(at) + np.linspace(-20, 20, 21)[:, None]
        return xy, sc

    return body


def _rtmw(body, hand):
    m = RtmwHands.__new__(RtmwHands)
    m.body, m.hand = body, hand
    return m


def test_rtmw_sides_from_the_body_and_refined_on_the_same_hand():
    def hand(_rgb, box, padding):  # a close look: the hand, 1 px to the right
        c = np.array([(box[0] + box[2]) / 2 + 1, (box[1] + box[3]) / 2])
        return c + np.linspace(-20, 20, 21)[:, None], np.full(21, 0.8)

    person = {"subject_id": 4, "confidence": 0.9, "bbox_xyxy": [0, 0, 600, 600]}
    left, right = _rtmw(_wholebody((100, 300), (400, 300)), hand)(None, [person])
    assert (left["subject_id"], left["side"], right["side"]) == (4, "left", "right")
    assert np.mean(left["keypoints"], axis=0) == pytest.approx((101, 300))
    assert left["score"] == pytest.approx(0.8)


def test_rtmw_keeps_its_own_points_when_the_close_look_finds_the_other_hand():
    def other(_rgb, _box, padding):  # always the hand at (400, 300)
        return np.array([400.0, 300.0]) + np.linspace(-20, 20, 21)[:, None], np.full(21, 0.8)

    person = {"subject_id": 4, "confidence": 0.9, "bbox_xyxy": [0, 0, 600, 600]}
    hands = _rtmw(_wholebody((100, 300), (400, 300)), other)(None, [person])
    by_side = {h["side"]: np.mean(h["keypoints"], axis=0) for h in hands}
    assert by_side["left"] == pytest.approx((100, 300))  # RTMW's, not the other hand
    assert by_side["right"] == pytest.approx((400, 300))


def test_rtmw_drops_weak_hands():
    def hand(_rgb, box, padding):
        c = np.array([(box[0] + box[2]) / 2, (box[1] + box[3]) / 2])
        return c + np.linspace(-20, 20, 21)[:, None], np.full(21, 0.2)

    person = {"subject_id": 4, "confidence": 0.9, "bbox_xyxy": [0, 0, 600, 600]}
    assert _rtmw(_wholebody((100, 300), (400, 300), score=0.1), hand)(None, [person]) == []
    assert _rtmw(_wholebody((100, 300), (400, 300)), hand)(None, [person]) == []


def test_untracked_pose_people_are_saved_as_nobody():
    h = dict(_hand((0, 0)), subject_id=-2, side="left")
    assert run_hands.rounded(h)["subject_id"] is None
    assert run_hands.rounded(dict(h, subject_id=3))["subject_id"] == 3


def test_seen_spans_and_summary():
    assert seen_spans([0, 40, 80, 2000, 2040], 500) == [[0, 80], [2000, 2040]]
    assert seen_spans([], 500) == []
    h = {"subject_id": 1, "side": "left", "score": 0.5, "keypoints": _hand((0, 0))["keypoints"]}
    frames = [{"timestamp_ms": 40.0 * k, "hands": [h] if k < 5 else []} for k in range(10)]
    (s,) = summarise(frames, 40.0)
    assert (s["subject_id"], s["side"], s["frames"]) == (1, "left", 5)
    assert s["seen_ms"] == 200.0 and s["seen_spans"] == [[0.0, 200.0]]
    assert s["median_size_px"] == pytest.approx(80, abs=0.1) and s["mean_score"] == 0.5


def _session(root: Path, n: int = 12) -> Path:
    cv2 = pytest.importorskip("cv2")
    s = root / "sess"
    (s / "video").mkdir(parents=True)
    (s / "pose").mkdir()
    for cam in (1, 2):
        video = s / "video" / f"video_{cam}.mp4"
        out = cv2.VideoWriter(str(video), cv2.VideoWriter_fourcc(*"mp4v"), 10.0, (320, 240))
        for _ in range(n):
            out.write(np.full((240, 320, 3), 40, np.uint8))
        out.release()
        if video.stat().st_size == 0:
            pytest.skip("this OpenCV cannot write mp4")
    lines = ["frame_id,elapsed_ns"] + [f"{k + 1},{(5000 + 100 * k) * 1_000_000}" for k in range(n)]
    (s / "video" / "timestamps_cam1.csv").write_text("\n".join(lines) + "\n")
    # Pose on camera 1 only, every second frame; nobody in the last two.
    frames = [
        {"frame_index": k, "subjects": [_person(7, left=(100, 150))] if k < n - 2 else []}
        for k in range(0, n, 2)
    ]
    (s / "pose" / "video_1.yolov8n-pose.pose.json").write_text(json.dumps({"frames": frames}))
    return s


def fake_estimator(rgb, people):
    """One left hand at each person's left wrist."""
    hands = []
    for p in people:
        h = _hand(tuple(p["keypoints"][9]), size=60)
        h.update(subject_id=p["subject_id"], side="left")
        hands.append(h)
    return hands


def test_runner_writes_hands_frames_and_video(tmp_path, capsys):
    s = _session(tmp_path)
    assert run_hands.main(["--session", str(s), "--model", "mediapipe"], fake_estimator) == 0
    out = capsys.readouterr().out
    assert "Camera 1/2: video_1.mp4" in out and "  100.0%  (2/2)" in out
    assert "video_2.mp4: no Pose result for this camera" in out
    res = json.loads((s / "hands" / "video_1.hands.json").read_text())
    assert res["schema"] == "mosaic-hands-v1" and res["model"] == "mediapipe"
    assert res["camera_index"] == 1 and res["video_start_ms"] == 5000.0
    assert len(res["keypoint_names"]) == 21 and "frames" not in res
    (hand,) = res["hands"]
    # Frames 0-9 have the person (frames 1, 3 ... take the Pose frame before).
    assert (hand["subject_id"], hand["side"], hand["frames"]) == (7, "left", 10)
    assert res["annotated_video"] == "hands/video_1.hands.mp4"
    assert (s / res["annotated_video"]).stat().st_size > 0
    frames = json.loads((s / res["frames_file"]).read_text())["frames"]
    assert len(frames) == 12 and frames[-1]["hands"] == []
    assert len(frames[0]["hands"][0]["keypoints"]) == 21
    assert not (s / "hands" / "video_2.hands.json").exists()


def test_runner_stride_and_no_video(tmp_path):
    s = _session(tmp_path)
    args = ["--session", str(s), "--camera", "1", "--stride", "3", "--no-video"]
    assert run_hands.main(args, fake_estimator) == 0
    res = json.loads((s / "hands" / "video_1.hands.json").read_text())
    assert res["stride"] == 3 and "annotated_video" not in res
    frames = json.loads((s / res["frames_file"]).read_text())["frames"]
    assert [f["frame_index"] for f in frames] == [0, 3, 6, 9]


def test_runner_fails_when_no_camera_has_pose(tmp_path):
    s = _session(tmp_path)
    assert run_hands.main(["--session", str(s), "--camera", "2"], fake_estimator) == 1
