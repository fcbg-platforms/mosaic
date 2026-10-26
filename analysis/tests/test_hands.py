"""Hands (hands/, run_hands.py) without the models: whose hand and which side,
repeats, when each hand is seen, and the runner with a stand-in model."""

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_hands
from hands import hands3d, kinematics, movements
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


def _flat_hand(offset=(0.0, 0.0)):
    """A straight, open hand, palm 100 px: wrist at the origin, the middle
    knuckle 100 px up, fingers straight up from their knuckles, the thumb
    tip 60 px right of the index tip (grip aperture 0.6 palm)."""
    p = np.zeros((21, 2))
    for f, (_name, chain) in enumerate(kinematics.FINGERS.items()):
        x = -40.0 + 20.0 * f  # the middle finger (f = 2) straight above the wrist
        for j, k in enumerate(chain):
            p[k] = (x, -100.0 - 20.0 * j)
    p[4] = p[8] + (60.0, 0.0)  # the thumb tip, off its line: a bent thumb
    return p + np.asarray(offset)


def _moving(n, fps, speed_px_s, gap=None, spike=None):
    """``n`` frames of the flat hand moving right at ``speed_px_s``; frames
    in ``gap`` (a range) left out; at frame ``spike`` the index tip jumps
    (the palm stays: only the point-by-point check can catch it)."""
    out = []
    for i in range(n):
        if gap is not None and i in gap:
            continue
        pts = _flat_hand((speed_px_s * i / fps, 0.0))
        if i == spike:
            pts[8] += (300.0, 0.0)
        out.append((1000.0 * i / fps, pts.tolist(), [0.9] * 21))
    return out


def test_kinematics_speed_in_hand_lengths_aperture_and_bend():
    s = kinematics.hand_series(_moving(50, 25.0, 200.0), 25.0)
    mid = slice(5, -5)  # away from the filter's ends
    assert np.nanmedian(s["palm_px"]) == pytest.approx(100.0)
    assert s["wrist_speed_px_s"][mid] == pytest.approx(200.0, rel=0.02)
    assert s["wrist_speed_hl_s"][mid] == pytest.approx(2.0, rel=0.02)
    assert s["wrist_acceleration_hl_s2"][mid] == pytest.approx(0.0, abs=0.2)
    assert s["grip_aperture_palm"][mid] == pytest.approx(0.6, rel=0.01)
    assert s["index_bend"][mid] == pytest.approx(0.0, abs=1e-6)  # straight
    assert np.nanmean(s["thumb_bend"]) > 0.1
    summ = kinematics.summary(s, 25.0)
    assert summ["wrist_speed_median_hl_s"] == pytest.approx(2.0, rel=0.02)
    assert summ["measured_s"] == pytest.approx(2.0, abs=0.05)
    assert summ["wrist_path_hl"] == pytest.approx(4.0, rel=0.05)


def test_kinematics_drops_a_jump_and_splits_at_long_gaps():
    s = kinematics.hand_series(_moving(50, 25.0, 200.0, spike=25), 25.0)
    # The one-frame jump is dropped and filled, not a burst of speed.
    assert np.nanmax(s["index_tip_speed_hl_s"][5:-5]) < 2.6
    short = kinematics.hand_series(_moving(50, 25.0, 200.0, gap=range(20, 24)), 25.0)
    assert len(short["time_ms"]) == 50  # 4 frames (0.16 s): filled
    long = kinematics.hand_series(_moving(60, 25.0, 200.0, gap=range(20, 30)), 25.0)
    # 0.4 s: two runs, the gap not filled, nothing held over it.
    assert not np.any((long["time_ms"] > 800.0) & (long["time_ms"] < 1200.0))
    assert kinematics.hand_series(_moving(10, 25.0, 200.0), 25.0) is None  # 0.4 s: too short


def test_kinematics_leaves_out_doubtful_points_and_unknown_hands():
    smp = _moving(50, 25.0, 200.0)
    smp[30] = (smp[30][0], smp[30][1], [0.9] * 8 + [0.1] + [0.9] * 12)  # index tip unsure
    s = kinematics.hand_series(smp, 25.0)
    assert np.all(np.isfinite(s["grip_aperture_palm"][5:-5]))  # filled from its neighbours
    # Unsure for 0.4 s while the rest is seen: not filled, not held.
    for i in range(20, 30):
        smp[i] = (smp[i][0], smp[i][1], [0.9] * 8 + [0.1] + [0.9] * 12)
    s = kinematics.hand_series(smp, 25.0)
    assert np.all(np.isfinite(s["wrist_speed_hl_s"][20:30]))
    assert np.all(np.isnan(s["grip_aperture_palm"][20:30]))
    frames = [
        {"timestamp_ms": 0.0, "hands": [
            {"subject_id": 1, "side": "left", "keypoints": [], "scores": []},
            {"subject_id": None, "side": "left", "keypoints": [], "scores": []},
            {"subject_id": 2, "side": "unknown", "keypoints": [], "scores": []},
        ]},
    ]  # fmt: skip
    assert list(kinematics.hand_samples(frames)) == [(1, "left")]


def test_runner_kinematics_and_kinematics_only(tmp_path):
    s = _session(tmp_path)
    assert run_hands.main(["--session", str(s), "--camera", "1", "--no-video"], fake_estimator) == 0
    res = json.loads((s / "hands" / "video_1.hands.json").read_text())
    assert res["kinematics_file"] == "hands/video_1.hands.kinematics.csv"
    (hand,) = res["hands"]
    # The fake hand stands still at the wrist: no speed.
    assert hand["kinematics"]["wrist_speed_median_hl_s"] == pytest.approx(0.0, abs=1e-6)
    lines = (s / res["kinematics_file"]).read_text().splitlines()
    assert lines[0].startswith("time_ms,subject_id,side,wrist_x_px") and len(lines) == 11
    # Again from the saved points, without a model.
    (s / res["kinematics_file"]).unlink()
    assert run_hands.main(["--session", str(s), "--camera", "1", "--kinematics-only"]) == 0
    assert (s / res["kinematics_file"]).exists()
    assert run_hands.main(["--session", str(s), "--camera", "2", "--kinematics-only"]) == 1


def test_kinematics_at_every_nth_frame_with_uneven_timestamps():
    rng = np.random.default_rng(1)
    smp = _moving(100, 21.0, 200.0)  # 21 fps, about 4.8 s
    jittered = [(t + rng.uniform(-8, 8), p, sc) for t, p, sc in smp]
    for stride in (1, 2):
        rate = 21.0 / stride
        s = kinematics.hand_series(jittered[::stride], rate)
        summ = kinematics.summary(s, rate)
        # Nearly all of it measured, at the right speed, whatever the stride.
        assert summ["measured_s"] == pytest.approx(4.7, abs=0.3)
        assert summ["wrist_speed_median_hl_s"] == pytest.approx(2.0, rel=0.05)
    # Under 10 analysed frames a second: too few for speeds.
    assert kinematics.hand_series(jittered[::3], 7.0) is None
    # At 10 a second, one frame in three missed: gaps of about 0.2 s are
    # still filled, as the step itself is 0.1 s.
    ten = _moving(60, 10.0, 200.0)
    sparse = [(t + rng.uniform(-10, 10), p, sc) for i, (t, p, sc) in enumerate(ten) if i % 3 != 2]
    summ = kinematics.summary(kinematics.hand_series(sparse, 10.0), 10.0)
    assert summ["measured_s"] == pytest.approx(5.9, abs=0.3)


def test_kinematics_an_unsure_wrist_keeps_the_other_points():
    unsure = [0.1] + [0.9] * 20
    smp = [(t, p, unsure) for t, p, _sc in _moving(50, 25.0, 200.0)]
    # The wrist never measured: nothing to give.
    assert kinematics.hand_series(smp, 25.0) is None
    # Unsure for 0.4 s: the fingers are still measured then.
    smp = [(t, p, unsure if 20 <= i < 30 else [0.9] * 21) for i, (t, p, _s) in enumerate(smp)]
    s = kinematics.hand_series(smp, 25.0)
    assert np.isfinite(s["index_bend"][20:30]).all()
    assert np.isnan(s["wrist_speed_px_s"][21:29]).all()


def test_resample_keeps_a_sample_after_a_long_gap():
    t = np.array([0.0, 0.04, 0.08, 0.5, 0.54])
    xy = np.arange(5, dtype=float).reshape(5, 1, 1)
    out = kinematics.resample(t, xy, t)
    assert out[:, 0, 0].tolist() == [0.0, 1.0, 2.0, 3.0, 4.0]
    mid = kinematics.resample(t, xy, np.array([0.3]))
    assert np.isnan(mid[0, 0, 0])  # inside the long gap: not filled


def test_runner_keeps_the_points_when_kinematics_fail(tmp_path, monkeypatch, capsys):
    s = _session(tmp_path)

    def broken(*_args):
        raise ValueError("boom")

    monkeypatch.setattr(run_hands.kinematics, "hand_series", broken)
    assert run_hands.main(["--session", str(s), "--camera", "1", "--no-video"], fake_estimator) == 0
    res = json.loads((s / "hands" / "video_1.hands.json").read_text())
    assert "kinematics_file" not in res and "kinematics" not in res["hands"][0]
    assert "kinematics failed" in capsys.readouterr().out


def _reach_series(rate=25.0, reaches=((0.5, 1.0, 5.0),), total_s=4.0):
    """Wrist speed and position for minimum-jerk reaches along x, each
    ``(start s, duration s, distance in hand lengths)``, palm 100 px; the
    grip aperture opens to 1.0 palm at 60% of each reach."""
    t = np.arange(0.0, total_s, 1.0 / rate)
    speed = np.zeros_like(t)
    aperture = np.full_like(t, 0.3)
    for start, dur, dist in reaches:
        tau = (t - start) / dur
        on = (tau >= 0) & (tau <= 1)
        speed[on] += dist / dur * 30 * tau[on] ** 2 * (1 - tau[on]) ** 2
        aperture[on] = 0.3 + 0.7 * np.exp(-(((tau[on] - 0.6) / 0.15) ** 2))
    x = np.cumsum(speed) / rate * 100.0
    return {
        "time_ms": t * 1000.0,
        "wrist_speed_hl_s": speed,
        "wrist_x_px": x,
        "wrist_y_px": np.zeros_like(t),
        "grip_aperture_palm": aperture,
    }


def test_one_smooth_reach():
    (m,), cut_off = movements.find_movements(_reach_series(), 25.0)
    assert cut_off == []
    # Above 0.5 hand lengths/s of a 9.4 peak: about 0.84 of the second.
    assert m["start_ms"] == pytest.approx(560, abs=40)
    assert m["duration_s"] == pytest.approx(m["end_ms"] / 1000 - m["start_ms"] / 1000)
    assert m["duration_s"] == pytest.approx(0.84, abs=0.05)
    assert m["peak_speed_hl_s"] == pytest.approx(9.375, rel=0.01)
    assert m["time_to_peak_pct"] == pytest.approx(50, abs=5)
    assert m["movement_units"] == 1 and m["straightness"] == pytest.approx(1.0)
    assert m["path_hl"] == pytest.approx(5.0, rel=0.05)
    assert m["max_grip_aperture_palm"] == pytest.approx(1.0, abs=0.02)
    assert m["time_to_max_aperture_pct"] == pytest.approx(60, abs=6)
    assert m["sparc"] == pytest.approx(-1.41, abs=0.05)


def test_sparc_and_units_see_stops_and_starts():
    rate = 100.0
    t = np.arange(0, 1.0 + 1e-9, 1 / rate)
    smooth = 30 * t**2 * (1 - t) ** 2
    assert movements.sparc(smooth, rate) == pytest.approx(-1.406, abs=0.01)
    assert movements.sparc(smooth, 25.0) == pytest.approx(-1.406, abs=0.01)  # any rate
    jerky = np.r_[smooth, 0.6 * smooth]
    assert movements.sparc(jerky, rate) < -1.8 and movements.movement_units(jerky) == 2
    # A wobble under 0.2 hand lengths/s is not a unit of its own.
    assert movements.movement_units(smooth + 0.05 * np.sin(40 * t)) == 1


def test_movements_need_speed_duration_and_no_gap():
    rate = 25.0
    two = _reach_series(reaches=((0.5, 1.0, 5.0), (2.5, 1.0, 5.0)))
    assert len(movements.find_movements(two, rate)[0]) == 2
    slow = _reach_series(reaches=((0.5, 2.0, 1.5),))  # peaks at 1.1: too slow
    assert movements.find_movements(slow, rate) == ([], [])
    # The hand not measured mid-reach: two cut-off halves, left out.
    gap = _reach_series()
    gap["wrist_speed_hl_s"][25:28] = np.nan
    found, cut_off = movements.find_movements(gap, rate)
    assert found == [] and len(cut_off) == 2
    jump = _reach_series()
    jump["time_ms"][30:] += 2000.0  # two runs of the hand, not one movement
    found, cut_off = movements.find_movements(jump, rate)
    assert found == [] and len(cut_off) == 2
    assert cut_off[0]["end_ms"] < cut_off[1]["start_ms"]
    # Seen already moving: left out; the next reach, whole, is kept.
    late = _reach_series(reaches=((0.5, 1.0, 5.0), (2.5, 1.0, 5.0)))
    late = {k: v[20:] for k, v in late.items()}
    found, cut_off = movements.find_movements(late, rate)
    assert len(found) == 1 and len(cut_off) == 1 and found[0]["start_ms"] > 2000


def test_a_slow_smooth_reach_is_as_smooth_as_a_fast_one():
    rate = 25.0
    (fast,), _ = movements.find_movements(_reach_series(), rate)
    (slow,), _ = movements.find_movements(_reach_series(reaches=((0.5, 1.0, 0.85),)), rate)
    assert slow["peak_speed_hl_s"] == pytest.approx(1.59, abs=0.05)
    assert slow["sparc"] == pytest.approx(fast["sparc"], abs=0.02)
    assert slow["movement_units"] == 1
    assert movements.sparc(np.zeros(10), rate) == 0.0


def test_hand_and_person_summaries():
    rate = 25.0
    found, _ = movements.find_movements(
        _reach_series(reaches=((0.5, 1.0, 5.0), (2.5, 1.0, 2.0))), rate
    )
    summ = movements.hand_summary(found, 3)
    assert summ["movements"] == 2 and summ["cut_off"] == 3
    assert summ["movement_units_median"] == 1.0
    assert summ["peak_speed_median_hl_s"] == pytest.approx((9.375 + 3.75) / 2, rel=0.02)
    assert movements.hand_summary([])["movements"] == 0
    left = [{"start_ms": 0.0, "end_ms": 1000.0}]
    right = [{"start_ms": 500.0, "end_ms": 1500.0}, {"start_ms": 3000.0, "end_ms": 3500.0}]
    p = movements.person_summary(left, right)
    assert p["laterality_index"] == pytest.approx(1 / 3, abs=0.01)
    assert p["both_moving_s"] == 0.5 and p["both_moving_share"] == pytest.approx(0.25)
    assert movements.person_summary([], [])["laterality_index"] is None


def test_runner_writes_movements_and_people(tmp_path):
    s = _session(tmp_path)
    assert run_hands.main(["--session", str(s), "--camera", "1", "--no-video"], fake_estimator) == 0
    res = json.loads((s / "hands" / "video_1.hands.json").read_text())
    assert res["movements_file"] == "hands/video_1.hands.movements.csv"
    head = (s / res["movements_file"]).read_text().splitlines()[0]
    assert head.startswith("subject_id,side,start_ms,end_ms,duration_s,peak_speed_hl_s")
    # The fake hand is still: no movement, and one hand: no people entry.
    assert res["hands"][0]["movements"]["movements"] == 0 and res["people"] == []

    def two_hands(rgb, people):
        hands = fake_estimator(rgb, people)
        for h in list(hands):
            right = _hand(tuple(np.add(h["keypoints"][0], (150, 0))), size=60)
            right.update(subject_id=h["subject_id"], side="right")
            hands.append(right)
        return hands

    assert run_hands.main(["--session", str(s), "--camera", "1", "--no-video"], two_hands) == 0
    res = json.loads((s / "hands" / "video_1.hands.json").read_text())
    (person,) = res["people"]
    assert person["subject_id"] == 7 and person["laterality_index"] is None  # no movement
    assert person["both_moving_s"] == 0.0


def _room_camera(index, position):
    """A 1920 x 1080 camera, focal length 1400 px, at ``position`` (mm)
    looking at the room origin."""
    from pose3d.triangulation import CameraGeom
    from test_pose3d_triangulation import _look_at_camera

    k = np.array([[1400.0, 0, 960.0], [0, 1400.0, 540.0], [0, 0, 1]])
    return CameraGeom(
        index=index,
        camera_matrix=k,
        dist_coeffs=np.zeros(5),
        extrinsic_rt=_look_at_camera(position),
    )


def _hand_3d(offset=(0.0, 0.0, 0.0)):
    """A flat open hand in the room, palm 100 mm, around ``offset``."""
    p = np.zeros((21, 3))
    p[:, :2] = _flat_hand()  # pixels of the flat hand used as millimetres
    return p + np.asarray(offset)


def _seen(points, cam, side, subject, shift=(0.0, 0.0)):
    from pose3d.triangulation import project_point_px

    kp = [list(project_point_px(p, cam) + shift) for p in points]
    return {"side": side, "subject_id": subject, "keypoints": kp, "scores": [0.9] * 21}


def test_hands3d_triangulates_two_cameras_and_reports_their_agreement():
    cams = {0: _room_camera(0, (-800, -600, -1300)), 4: _room_camera(4, (900, -500, -1200))}
    left, right = _hand_3d((-150, 0, 0)), _hand_3d((150, 0, 0))
    per = {
        0: [_seen(left, cams[0], "left", 3), _seen(right, cams[0], "right", 3)],
        4: [
            _seen(right, cams[4], "right", 7),
            _seen(left, cams[4], "left", 7, shift=(0, 12)),
        ],
    }
    found, compared, matched = hands3d.match_tick(per, cams)
    assert compared == 2 and matched == 2
    by_side = {h["side"]: h for h in found}
    # The right hand, seen alike by both: back to the millimetre, and the
    # two cameras' lines of sight through its points meet.
    assert np.abs(by_side["right"]["points"] - right).max() < 0.5
    assert np.linalg.norm(by_side["right"]["gaps"][(0, 4)]) < 0.1
    # The left one, Camera 5 shifted 12 px across the line between the
    # cameras (a calibration that is off): the lines of sight miss by about
    # 12 px at that camera's distance (1.4 m at 1400 px: about 12 mm), all
    # of it steady.
    assert np.abs(by_side["left"]["points"] - left).max() < 15.0
    for h in found:
        h["frame_gap_ms"] = {(0, 4): 5.0}
    report = hands3d.pair_report([(0.0, [by_side["left"]])], compared, matched)
    (pair,) = report["camera_pairs"]
    assert pair["cameras"] == [0, 4] and pair["hands"] == 1
    assert pair["gap_median_mm"] == pytest.approx(12.0, abs=3.0)
    assert pair["offset_mm"] == pytest.approx(pair["gap_median_mm"], abs=0.5)
    assert pair["scatter_median_mm"] < 0.5 and pair["frame_gap_median_ms"] == 5.0
    assert pair["offset_same_moment_mm"] is None  # under 20 hands at once
    many = hands3d.pair_report([(0.0, [by_side["left"]] * 20)], compared, matched)
    assert many["camera_pairs"][0]["offset_same_moment_mm"] == pytest.approx(
        pair["offset_mm"], abs=0.1
    )
    assert report["views_compared"] == 2 and report["views_matched"] == 2


def test_hands3d_does_not_join_views_that_disagree():
    cams = {0: _room_camera(0, (-800, -600, -1300)), 4: _room_camera(4, (900, -500, -1200))}
    hand = _hand_3d()
    per = {
        0: [_seen(hand, cams[0], "left", 3)],
        4: [_seen(_hand_3d((0, 300, 0)), cams[4], "left", 7)],  # another hand
    }
    found, compared, matched = hands3d.match_tick(per, cams)
    assert found == [] and compared == 1 and matched == 0
    # Another side is never the same hand.
    per[4] = [_seen(hand, cams[4], "right", 7)]
    assert hands3d.match_tick(per, cams)[0] == []


def test_hands3d_tracks_a_hand_over_time():
    def h(side, x):
        return {"side": side, "points": _hand_3d((x, 0, 0))}

    ticks = [
        (0.0, [h("left", 0), h("right", 400)]),
        (40.0, [h("right", 410), h("left", 10)]),
        (1000.0, [h("left", 900)]),  # 0.96 s later, 0.9 m away: under 2 m/s
        (4000.0, [h("left", 900)]),  # over 2 s later: a new hand
    ]
    hands3d.track(ticks)
    ids = [[x["hand"] for x in found] for _t, found in ticks]
    assert ids[0] == [1, 2] and ids[1] == [2, 1] and ids[2] == [1] and ids[3] == [3]
    # No point found: a number of its own, nothing to follow.
    empty = [(0.0, [{"side": "left", "points": np.full((21, 3), np.nan)}])]
    assert hands3d.track(empty)[0][1][0]["hand"] == 1


def test_hands3d_a_third_camera_joins_with_its_best_view():
    cams = {
        0: _room_camera(0, (-800, -600, -1300)),
        2: _room_camera(2, (0, -900, -1500)),
        4: _room_camera(4, (900, -500, -1200)),
    }
    hand = _hand_3d()
    per = {
        0: [_seen(hand, cams[0], "left", 3)],
        2: [
            # Both a little off, so Cameras 1 and 5 are the pair it joins;
            # the second agrees better.
            _seen(hand, cams[2], "left", 1, shift=(0, 6)),
            _seen(hand, cams[2], "left", 2, shift=(0, 2)),
        ],
        4: [_seen(hand, cams[4], "left", 7)],
    }
    (h,), _compared, matched = hands3d.match_tick(per, cams)
    assert matched == 1 and sorted(h["views"]) == [0, 2, 4]
    assert h["views"][2]["subject_id"] == 2  # the better of camera 3's two views
    # Within a millimetre or two (camera 3 is 2 px off on purpose).
    assert np.median(np.linalg.norm(h["points"] - hand, axis=1)) < 2.0
    assert sorted(h["gaps"]) == [(0, 2), (0, 4), (2, 4)]


def test_camera_hands_finds_the_frame_a_tick_names(tmp_path):
    ts = tmp_path / "timestamps_cam0.csv"
    ts.write_text("frame_id,elapsed_ns\n" + "".join(f"{i + 10},{i}\n" for i in range(10)))
    hand = {"side": "left", "subject_id": 1, "keypoints": [], "scores": []}
    nobody = {"side": "right", "subject_id": None, "keypoints": [], "scores": []}
    unknown = {"side": "unknown", "subject_id": None, "keypoints": [], "scores": []}
    frames = [{"frame_index": i, "hands": [hand, nobody, unknown]} for i in range(0, 10, 2)]
    ch = hands3d.CameraHands(frames, ts)
    # Decoded frame 4, analysed: its hands of a known side, a person or not.
    assert ch.at_frame_id(14) == [hand, nobody]
    assert ch.at_frame_id(15) is None  # decoded 5: not analysed (every 2nd frame)
    assert ch.at_frame_id(99) is None  # not in the timestamps: no guess
    guessed = hands3d.CameraHands(frames, tmp_path / "missing.csv")
    assert guessed.at_frame_id(5) == [hand, nobody]  # no timestamps: id N is frame N - 1


def test_kinematics_in_millimetres():
    rate = 25.0
    smp = [(1000.0 * i / rate, _hand_3d((300.0 * i / rate, 0, 0)), [1.0] * 21) for i in range(50)]
    s = kinematics.hand_series(smp, rate, unit="mm")
    mid = slice(5, -5)
    assert s["wrist_speed_mm_s"][mid] == pytest.approx(300.0, rel=0.02)
    assert s["wrist_speed_hl_s"][mid] == pytest.approx(3.0, rel=0.02)  # palm 100 mm
    assert s["grip_aperture_mm"][mid] == pytest.approx(60.0, rel=0.01)
    assert "wrist_z_mm" in s and "wrist_x_px" not in s
    summ = kinematics.summary(s, rate)
    assert summ["palm_median_mm"] == pytest.approx(100.0, abs=0.5)
    assert summ["wrist_speed_median_mm_s"] == pytest.approx(300.0, rel=0.02)


def test_runner_puts_the_hands_two_cameras_see_in_the_room(tmp_path):
    from pose3d.triangulation import project_point_px

    cams = {0: _room_camera(0, (-800, -600, -1300)), 4: _room_camera(4, (900, -500, -1200))}
    s = tmp_path / "sess"
    (s / "hands").mkdir(parents=True)
    (s / "video").mkdir()
    meta = {"cameras": []}
    for c, g in cams.items():
        meta["cameras"].append(
            {
                "index": c,
                "calibration": {
                    "calibrated": True,
                    "extrinsic_calibrated": True,
                    "camera_matrix": g.camera_matrix.ravel().tolist(),
                    "dist_coeffs": g.dist_coeffs.tolist(),
                    "extrinsic_rt": g.extrinsic_rt.ravel().tolist(),
                },
            }
        )
    (s / "session_meta.json").write_text(json.dumps(meta))
    n, rate = 40, 25.0
    manifest = {
        "master_fps": rate,
        "step_ns": "40000000",
        "t_origin_ns": "5000000000",
        "total_ticks": n,
        "ticks": {
            **{f"cam{c}_frame_ids": list(range(1, n + 1)) for c in cams},
            "cam0_delta_ms": [1.0] * n,
            "cam4_delta_ms": [-2.0] * n,
        },
    }
    (s / "sync_manifest.json").write_text(json.dumps(manifest))
    for c, g in cams.items():
        lines = "".join(f"{i + 1},{i}\n" for i in range(n))
        (s / "video" / f"timestamps_cam{c}.csv").write_text("frame_id,elapsed_ns\n" + lines)
        frames = []
        for i in range(n):  # the left hand moving right at 250 mm/s
            pts = _hand_3d((250.0 * i / rate, 0, 0))
            kp = [list(project_point_px(p, g)) for p in pts]
            frames.append(
                {
                    "frame_index": i,
                    "hands": [
                        {"subject_id": 2, "side": "left", "keypoints": kp, "scores": [0.9] * 21}
                    ],
                }
            )
        (s / "hands" / f"video_{c}.hands.json").write_text(json.dumps({"stride": 1}))
        (s / "hands" / f"video_{c}.hands.frames.json").write_text(json.dumps({"frames": frames}))
    path = run_hands.add_hands3d(s)
    res = json.loads(path.read_text())
    assert res["schema"] == "mosaic-hands3d-v1"
    (hand,) = res["hands"]
    assert (hand["subject_id"], hand["side"], hand["cameras"], hand["ticks"]) == (
        2,
        "left",
        [0, 4],
        n,
    )
    assert hand["kinematics"]["wrist_speed_median_mm_s"] == pytest.approx(250.0, rel=0.03)
    assert hand["kinematics"]["palm_median_mm"] == pytest.approx(100.0, abs=1.0)
    (pair,) = res["agreement"]["camera_pairs"]
    assert pair["cameras"] == [0, 4] and pair["gap_median_mm"] < 0.5
    assert pair["frame_gap_median_ms"] == 3.0
    assert res["master_fps"] == 25.0 and res["rate"] == 25.0
    head = (s / res["kinematics_file"]).read_text().splitlines()[0]
    assert head.startswith("time_ms,hand,subject_id,side,wrist_x_mm,wrist_y_mm,wrist_z_mm")
    frames3d = json.loads((s / res["frames_file"]).read_text())["frames"]
    assert len(frames3d) == n and frames3d[0]["timestamp_ms"] == 5000.0
    # A camera naming its last frame again (free-running slower than the
    # ticks): that tick is left out, not paired with the other's new frame.
    manifest["ticks"]["cam4_frame_ids"][5] = 5
    (s / "sync_manifest.json").write_text(json.dumps(manifest))
    res = json.loads(run_hands.add_hands3d(s).read_text())
    assert res["hands"][0]["ticks"] == n - 1
    # One calibrated camera only: nothing in 3D, and the old result is gone.
    meta["cameras"] = meta["cameras"][:1]
    (s / "session_meta.json").write_text(json.dumps(meta))
    assert run_hands.add_hands3d(s) is None
    assert not (s / "hands" / "hands3d.json").exists()
