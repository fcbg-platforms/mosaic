"""Object Tracking (objects/, run_objects.py) without the SAM 3 model: outlines,
the touch rule, contact runs, summaries, and the runner with a stand-in
tracker."""

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_objects
from objects.contact import contact_events, frame_contacts, outline, summarise, touches

SQUARE = [[100, 100], [200, 100], [200, 200], [100, 200]]


def test_outline_of_a_mask_and_of_nothing():
    m = np.zeros((300, 300), bool)
    m[100:201, 100:201] = True
    poly = outline(m)
    assert len(poly) == 4
    assert sorted(map(tuple, poly)) == [(100, 100), (100, 200), (200, 100), (200, 200)]
    assert outline(np.zeros((10, 10), bool)) == []


def test_touch_inside_near_and_far():
    assert touches(SQUARE, (150, 150), 0)
    assert touches(SQUARE, (210, 150), 12) and not touches(SQUARE, (230, 150), 12)
    assert not touches([], (150, 150), 50)


def _person(pid, wrist, vis=1.0):
    kp = np.zeros((17, 2))
    kp[9] = wrist
    v = np.zeros(17)
    v[9] = vis
    return {"subject_id": pid, "keypoints": kp.tolist(), "visibilities": v.tolist()}


def test_frame_contacts_need_a_seen_wrist_near_the_outline():
    objs = [{"id": 3, "polygon": SQUARE}]
    assert frame_contacts(objs, [_person(1, (205, 150))]) == [(3, 1)]  # within the margin
    assert frame_contacts(objs, [_person(1, (400, 150))]) == []
    assert frame_contacts(objs, [_person(1, (150, 150), vis=0.1)]) == []  # wrist not seen


def test_contact_runs_bridge_short_gaps_and_drop_blips():
    t = list(range(0, 2000, 100))  # 10 frames per second
    pairs = [[] for _ in t]
    for k in range(2, 8):
        pairs[k] = [(3, 1)]
    pairs[5] = []  # one missed frame: bridged
    pairs[15] = [(4, 2)]  # one frame alone: lasts one step, too short
    ev = contact_events(pairs, t, step_ms=100.0)
    # The last sample stands for the step after it.
    assert ev == [{"object_id": 3, "person": 1, "start_ms": 200, "end_ms": 800.0}]
    # Sparse samples (every 400 ms) still join into one contact.
    sparse = contact_events([[(3, 1)]] * 3, [0, 400, 800], step_ms=400.0)
    assert sparse == [{"object_id": 3, "person": 1, "start_ms": 0, "end_ms": 1200.0}]


def test_summary_counts_seen_travel_and_touch():
    frames = [
        {
            "timestamp_ms": 1000 + 100 * k,
            "objects": [{"id": 3, "label": "cup", "polygon": [[x + 10 * k, y] for x, y in SQUARE]}],
        }
        for k in range(5)
    ] + [{"timestamp_ms": 1500, "objects": [{"id": 3, "label": "cup", "polygon": []}]}]
    ev = [{"object_id": 3, "person": 1, "start_ms": 1000, "end_ms": 1400}]
    s = summarise(frames, ev, step_ms=100.0)[0]
    # The empty outline at 1500 ms is not "seen", and does not move the centre.
    assert s["seen_spans"] == [[1000, 1500.0]]
    assert s["label"] == "cup" and s["seen_pct"] == pytest.approx(83.3, abs=0.1)
    assert s["travel_px"] == pytest.approx(40.0) and s["mean_area_px"] == 10000.0
    assert s["contacts"] == 1 and s["touched_s_by_person"] == {"1": 0.4}


def test_camera_pick_keeps_the_picked_cameras_only():
    from camera_pick import pick_cameras

    vids = [Path("video/video_0.mp4"), Path("video/video_4.mp4"), Path("video/clip.mp4")]
    assert pick_cameras(vids, None) == vids
    assert pick_cameras(vids, [4]) == [vids[1]]
    assert run_objects.parse_args(["--session", "s", "--camera", "0", "4"]).camera == [0, 4]


def test_labels_are_split_and_deduplicated():
    assert run_objects.parse_labels(" toy, cup,,Toy , tablet ") == ["toy", "cup", "tablet"]


# ── The runner, with a stand-in for SAM 3 ────────────────────────────────────


def _session(root: Path, n: int = 30) -> Path:
    cv2 = pytest.importorskip("cv2")
    s = root / "sess"
    (s / "video").mkdir(parents=True)
    video = s / "video" / "video_1.mp4"
    out = cv2.VideoWriter(str(video), cv2.VideoWriter_fourcc(*"mp4v"), 10.0, (320, 240))
    for _ in range(n):
        out.write(np.full((240, 320, 3), 40, np.uint8))
    out.release()
    if video.stat().st_size == 0:
        pytest.skip("this OpenCV cannot write mp4")
    lines = ["frame_id,elapsed_ns"] + [f"{k + 1},{(5000 + 100 * k) * 1_000_000}" for k in range(n)]
    (s / "video" / "timestamps_cam1.csv").write_text("\n".join(lines) + "\n")
    # Pose: person 1's wrist on the cup from frame 10 to 19.
    frames = [
        {"frame_index": k, "subjects": [_person(1, (150, 150) if 10 <= k < 20 else (300, 30))]}
        for k in range(n)
    ]
    (s / "pose").mkdir()
    (s / "pose" / "video_1.n.pose.json").write_text(json.dumps({"frames": frames}))
    return s


def fake_track(video, labels, weights, stride):
    # As SAM 3's loader: frames stride - 1, 2 * stride - 1, ...
    for idx in range(stride - 1, 30, stride):
        objs = [{"id": 1, "label": labels[0], "score": 0.9, "polygon": SQUARE}]
        if idx < 5:  # a brief false detection, well under a second
            objs.append({"id": 2, "label": labels[0], "score": 0.7, "polygon": SQUARE[:3]})
        yield idx, objs


def test_runner_writes_objects_contacts_and_video(tmp_path):
    s = _session(tmp_path)
    assert run_objects.main(["--session", str(s), "--objects", "cup"], track=fake_track) == 0
    res = json.loads((s / "objects" / "video_1.objects.json").read_text())
    assert res["schema"] == "mosaic-objects-v1" and res["labels"] == ["cup"]
    assert "frames" not in res and res["video_start_ms"] == 5000.0
    frames = json.loads((s / res["frames_file"]).read_text())["frames"]
    assert len(frames) == 15 and frames[0]["frame_index"] == 1
    assert frames[0]["timestamp_ms"] == 5100.0
    # The wrist is on the cup in frames 10-19: analysed frames 11 to 19.
    (c,) = res["contacts"]
    assert c["person"] == 1 and c["start_ms"] == 6100.0 and c["end_ms"] == 7100.0
    assert res["summary"]["objects"][0]["touched_s_by_person"] == {"1": 1.0}
    assert (s / res["annotated_video"]).stat().st_size > 0
    # The brief object is left out of the results, but kept in the frames file.
    assert [o["id"] for o in res["summary"]["objects"]] == [1]
    assert res["summary"]["brief_objects_left_out"] == 1
    assert any(o["id"] == 2 for f in frames for o in f["objects"])


def test_runner_says_where_the_weights_go(tmp_path, capsys):
    s = _session(tmp_path)
    missing = str(tmp_path / "nowhere" / "sam3.pt")
    assert run_objects.main(["--session", str(s), "--weights", missing]) == 1
    assert "Download sam3.pt from Hugging Face" in capsys.readouterr().err


def test_contacts_survive_different_pose_and_object_steps(tmp_path):
    s = _session(tmp_path)
    # Pose analysed every 3rd frame only.
    d = json.loads((s / "pose" / "video_1.n.pose.json").read_text())
    d["frames"] = [f for f in d["frames"] if f["frame_index"] % 3 == 0]
    (s / "pose" / "video_1.n.pose.json").write_text(json.dumps(d))
    args = ["--session", str(s), "--objects", "cup", "--stride", "4", "--no-video"]
    assert run_objects.main(args, track=fake_track) == 0
    res = json.loads((s / "objects" / "video_1.objects.json").read_text())
    assert len(res["contacts"]) == 1 and res["contacts"][0]["person"] == 1
