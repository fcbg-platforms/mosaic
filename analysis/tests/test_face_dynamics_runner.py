"""
Tests for run_face_dynamics.py and face_dynamics/extract.py pieces that need
no model: frame times, head angles, intrinsics and the output files.
"""

import json
import math
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_face_dynamics as rfd
from face_dynamics.extract import head_angles, nominal_camera
from gaze.ray_math import rotation_matrix_from_rotvec


def test_frame_times_fill_missing_and_non_increasing_stamps():
    stamps = [1000.0, 1020.0, float("nan"), 1060.0, 1060.0]
    t = rfd.frame_times_ms(stamps, 7, fps=50.0)
    assert t.tolist() == [1000.0, 1020.0, 1040.0, 1060.0, 1080.0, 1100.0, 1120.0]
    assert rfd.frame_times_ms([], 3, fps=25.0).tolist() == [0.0, 40.0, 80.0]


def _r(yaw_deg=0.0, pitch_deg=0.0, roll_deg=0.0):
    """Camera-from-head rotation of a face turned from looking at the camera.

    Facing the camera the head frame (face along -z, +x the subject's left,
    which is image right) coincides with the camera frame.
    """
    # Yaw towards image right: a rotation about camera -y (image up axis).
    yaw = rotation_matrix_from_rotvec([0.0, -math.radians(yaw_deg), 0.0])
    pitch = rotation_matrix_from_rotvec([-math.radians(pitch_deg), 0.0, 0.0])
    roll = rotation_matrix_from_rotvec([0.0, 0.0, math.radians(roll_deg)])
    return roll @ yaw @ pitch


def test_head_angles_of_a_face_turning_and_tilting():
    assert head_angles(_r()) == pytest.approx((0.0, 0.0, 0.0), abs=1e-9)
    yaw, pitch, roll = head_angles(_r(yaw_deg=30.0))
    assert yaw == pytest.approx(30.0) and pitch == pytest.approx(0.0, abs=1e-9)
    yaw, pitch, _ = head_angles(_r(pitch_deg=20.0))
    assert pitch == pytest.approx(20.0) and yaw == pytest.approx(0.0, abs=1e-9)
    _, _, roll = head_angles(_r(roll_deg=10.0))
    assert roll == pytest.approx(10.0)


def test_nominal_and_calibrated_intrinsics(tmp_path):
    k, d, calibrated = rfd.camera_intrinsics(tmp_path, 0, 1280, 720)
    assert not calibrated
    assert np.allclose(k, nominal_camera(1280, 720)[0]) and k[0, 2] == 640.0
    meta = {
        "cameras": [
            {
                "index": 2,
                "offset_x": 100,
                "offset_y": 50,
                "calibration": {
                    "calibrated": True,
                    "camera_matrix": [1000, 0, 960, 0, 1000, 540, 0, 0, 1],
                    "dist_coeffs": [0.1, 0, 0, 0, 0],
                    "offset_x": 0,
                    "offset_y": 0,
                },
            }
        ]
    }
    (tmp_path / "session_meta.json").write_text(json.dumps(meta))
    k, d, calibrated = rfd.camera_intrinsics(tmp_path, 2, 1280, 720)
    assert calibrated
    assert (k[0, 2], k[1, 2]) == (860.0, 490.0)  # shifted by the recording's crop
    assert d[0] == 0.1


def _measurements(n=500, fps=50.0):
    """Synthetic face: open eyes with two blinks, one smile, face lost briefly."""
    from face_dynamics.metrics import EXPRESSIVE

    t = np.arange(n) / fps
    m = rfd.Measurements(n, 52)
    for i in range(n):
        if 300 <= i < 310:
            continue  # face lost
        blink = any(s <= t[i] < s + 0.16 for s in (2.0, 7.0))
        ear = 0.05 if blink else 0.3
        smile = 0.8 if 3.0 <= t[i] < 4.0 else 0.05
        shapes = {name: 0.05 for name in EXPRESSIVE}
        shapes.update(
            {
                "browInnerUp": 0.0,
                "browOuterUpLeft": 0.0,
                "browOuterUpRight": 0.0,
                "mouthSmileLeft": smile,
                "mouthSmileRight": smile,
            }
        )

        class Face:
            pass

        f = Face()
        f.ear_right = f.ear_left = ear
        f.yaw, f.pitch, f.roll = 2.0, -1.0, 0.0
        f.box = (100.0, 100.0, 300.0, 360.0)
        f.nose = (200.0, 230.0)
        f.outline = np.zeros((52, 2))
        f.blendshapes = shapes
        m.put(i, f)
    return m, t


def test_outputs_round_trip(tmp_path):
    m, t = _measurements()
    a = rfd.analyse(m, t)
    kinds = [e.kind for e in a["events"]]
    assert kinds.count("blink") == 2 and kinds.count("smile") == 1
    times_ms = 10_000.0 + t * 1000.0
    path = rfd.write_outputs(
        tmp_path, Path("video_3.mp4"), 3, 50.0, times_ms, m, a, {"min_confidence": 0.5}, None
    )
    doc = json.loads(path.read_text())
    assert doc["schema"] == rfd.SCHEMA
    assert doc["camera_index"] == 3 and doc["annotated_video"] is None
    assert len(doc["frames"]) == m.n
    assert doc["frames"][305] == {"frame_index": 305, "timestamp_ms": 16100, "face_detected": False}
    first = doc["frames"][0]
    assert first["face_box_px"] == [100, 100, 300, 360] and first["yaw"] == 2.0
    blink = next(e for e in doc["events"] if e["kind"] == "blink")
    assert blink["start_ms"] >= 12_000 and 100 <= blink["duration_ms"] <= 200
    assert doc["summary"]["blinks"]["count"] == 2
    assert (tmp_path / "video_3.face_dynamics.csv").read_text().count("\n") == m.n + 1
    events_csv = (tmp_path / "video_3.events.csv").read_text().splitlines()
    assert events_csv[0].startswith("kind,") and len(events_csv) == len(doc["events"]) + 1
