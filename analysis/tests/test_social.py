"""Social interaction (social/, run_social.py): synchrony found where it was
planted and not where it was not; mimicry likewise; the runner end to end."""

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_social
from social import signals
from social.mimicry import mimicry
from social.synchrony import synchrony

FS = 10.0


def smooth_noise(n: int, rng, width: int = 8) -> np.ndarray:
    k = np.ones(width) / width
    return np.convolve(rng.standard_normal(n + width), k, mode="valid")[:n]


def test_a_partner_who_follows_by_one_second_is_found():
    rng = np.random.default_rng(1)
    n = 3000  # 5 minutes at 10 per second
    a = smooth_noise(n, rng)
    b = np.roll(a, 10) + 0.3 * smooth_noise(n, rng)  # B does what A did 1 s earlier
    res = synchrony(a, b, FS, 5.0, 0.5, 3.0)
    s = res["summary"]
    assert s["mean_peak_r"] > 0.8 and s["p_value"] < 0.02 and s["synchrony"] > 0.2
    assert s["median_lag_s"] == pytest.approx(1.0, abs=0.15)
    assert s["a_leads_pct"] > 90
    # Swapped, B leads.
    s2 = synchrony(b, a, FS, 5.0, 0.5, 3.0)["summary"]
    assert s2["median_lag_s"] == pytest.approx(-1.0, abs=0.15) and s2["b_leads_pct"] > 90


def test_unrelated_people_are_not_in_synchrony():
    rng = np.random.default_rng(2)
    a, b = smooth_noise(3000, rng), smooth_noise(3000, rng)
    s = synchrony(a, b, FS, 5.0, 0.5, 3.0)["summary"]
    # Peak correlations are well above zero by chance alone; the surrogates
    # see that, so the pair does not stand out.
    assert s["mean_peak_r"] > 0.3
    assert s["p_value"] > 0.05 and abs(s["synchrony"]) < 0.05


def test_gaps_leave_windows_out():
    rng = np.random.default_rng(3)
    a = smooth_noise(1000, rng)
    b = a.copy()
    b[200:600] = np.nan  # B's face not seen for 40 s
    res = synchrony(a, b, FS, 5.0, 0.5, 3.0)
    t = np.array(res["series"]["t_s"])
    r = np.array([np.nan if v is None else v for v in res["series"]["peak_r"]])
    assert np.isnan(r[(t > 25) & (t < 55)]).all() and np.isfinite(r[t < 15]).all()


def test_mimicry_against_chance():
    rng = np.random.default_rng(4)
    a = np.sort(rng.uniform(0, 600, 40))
    follows = a + 1.0  # B smiles 1 s after each of A's smiles
    m = mimicry(a, follows, (0.0, 610.0))
    assert m["rate"] == 1.0 and m["chance_rate"] < 0.4 and m["p_value"] < 0.01
    unrelated = np.sort(rng.uniform(0, 600, 40))
    m2 = mimicry(a, unrelated, (0.0, 610.0))
    assert m2["p_value"] > 0.05
    assert mimicry([], follows, (0.0, 610.0))["rate"] is None


def test_on_grid_bins_and_fills_only_short_gaps():
    g = signals.on_grid([0, 50, 120, 330], [1.0, 3.0, 5.0, 7.0], 0.0, 5, 10.0)
    assert g[0] == 2.0 and g[1] == 5.0 and np.isnan(g[2]) and g[3] == 7.0
    hr = signals.on_grid([0, 2000, 10000], [60.0, 62.0, 70.0], 0.0, 11, 1.0, max_gap_s=5.0)
    assert hr[1] == 61.0 and np.isnan(hr[5]) and hr[10] == 70.0


# ── The runner ───────────────────────────────────────────────────────────────


def _session(root: Path, mode: str = "room", n_frames: int = 3000, face_b: bool = True) -> Path:
    s = root / "sess"
    (s / "video").mkdir(parents=True)
    (s / "session_meta.json").write_text(json.dumps({"recording": {"mode": mode}}))
    rng = np.random.default_rng(5)
    head = smooth_noise(n_frames, rng) * 10 + 20
    smiles = np.sort(rng.uniform(5, 290, 20))
    for cam, shift_ms in ((1, 0), (4, 800)):  # B follows A by 0.8 s
        (s / "video" / f"video_{cam}.mp4").write_bytes(b"")
        (s / "video" / f"timestamps_cam{cam}.csv").write_text("frame_id,elapsed_ns\n1,1000000000\n")
        frames = [
            {
                "timestamp_ms": 1000 + 100 * k + shift_ms,
                "face_detected": cam == 1 or face_b,
                "head_speed": float(head[k]),
            }
            for k in range(n_frames)
        ]
        events = [
            {"kind": "smile", "start_ms": 1000 + 1000 * t + shift_ms, "end_ms": 0} for t in smiles
        ]
        d = s / "face_dynamics"
        d.mkdir(exist_ok=True)
        (d / f"video_{cam}.face_dynamics.json").write_text(
            json.dumps({"schema": "mosaic-face-dynamics-v1", "frames": frames, "events": events})
        )
    return s


def test_runner_finds_the_planted_coupling(tmp_path):
    s = _session(tmp_path)
    assert run_social.main(["--session", str(s)]) == 0
    res = json.loads((s / "social" / "social.json").read_text())
    head = res["synchrony"]["head"]["summary"]
    assert head["p_value"] < 0.05 and head["median_lag_s"] == pytest.approx(0.8, abs=0.15)
    smile = res["mimicry"]["smile"]["a_to_b"]
    assert smile["rate"] == 1.0 and smile["p_value"] < 0.05
    # Pose and heart rate were not run: said so, per camera.
    assert any("run Pose on Camera 2 and Camera 5" in n for n in res["notes"])
    assert (s / "social" / "synchrony.csv").read_text().startswith("measure,t_ms")


def test_runner_refuses_interview_recordings_and_one_camera(tmp_path, capsys):
    s = _session(tmp_path, mode="interview")
    assert run_social.main(["--session", str(s)]) == 1
    assert "only one person is on camera" in capsys.readouterr().err
    s2 = _session(tmp_path / "b")
    assert run_social.main(["--session", str(s2), "--camera-a", "1", "--camera-b", "1"]) == 1


def test_runner_says_when_too_short_or_when_a_camera_sees_no_one(tmp_path):
    # Mimicry still works on a short recording; synchrony says why it does not
    # (14 s shared: B is 0.8 s behind).
    short = _session(tmp_path / "short", n_frames=150)  # 15 s
    assert run_social.main(["--session", str(short)]) == 0
    notes = json.loads((short / "social" / "social.json").read_text())["notes"]
    assert any("share only 14 s; at least 23 s are needed" in n for n in notes)
    away = _session(tmp_path / "away", face_b=False)  # Camera 5 never sees a face
    assert run_social.main(["--session", str(away)]) == 0
    notes = json.loads((away / "social" / "social.json").read_text())["notes"]
    assert any("Camera 5 measured it only 0% of the time" in n for n in notes)


def test_mimicry_on_a_short_shared_stretch_says_nothing_instead_of_failing():
    m = mimicry([1.0, 2.0], [1.5, 2.5], (0.0, 8.0))  # 8 s: too short for chance shifts
    assert m["rate"] is None and m["events"] == 2


def test_no_nan_in_the_saved_numbers(tmp_path):
    # A seen only in the first half, B only from 3 s before the middle: at lag
    # 0 no window has enough of both, at other lags some do.
    rng = np.random.default_rng(6)
    a = smooth_noise(1200, rng)
    b = np.roll(a, 20)
    a[600:] = np.nan
    b[:570] = np.nan
    res = synchrony(a, b, FS, 5.0, 0.5, 3.0)
    text = json.dumps(res)
    assert "NaN" not in text and res["summary"]["mean_lag0_r"] is None


def test_malformed_pose_and_pose_without_times(tmp_path):
    s = tmp_path / "s"
    (s / "pose").mkdir(parents=True)
    person = {"subject_id": 1, "bbox_xyxy": [0, 0, 100, 200], "visibilities": [1.0] * 17}
    frames = [
        {"timestamp_ns": 1_000_000_000 + k * 40_000_000, "subjects": [dict(person, keypoints=[])]}
        for k in range(5)
    ]
    (s / "pose" / "video_1.n.pose.json").write_text(json.dumps({"frames": frames}))
    b = signals.body_movement(s, 1)
    assert np.isnan(b["body_speed"]).all()  # no crash on keypoints missing
    for f in frames:
        del f["timestamp_ns"]
    (s / "pose" / "video_1.n.pose.json").write_text(json.dumps({"frames": frames}))
    b = signals.body_movement(s, 1)
    assert b is not None and b["t_ms"].size == 0  # ran, but nothing timed: not "not run"
