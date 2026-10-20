"""The audio-video clap check (sync_check/clap.py, run_clap_check.py): claps
heard at the right time, speech not taken for claps, hands meeting seen
between frames, and a known offset measured back."""

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_clap_check
from sync_check import clap


def _clicks(fs: int, seconds: float, times: list, seed: int = 0, echo: bool = False) -> np.ndarray:
    rng = np.random.default_rng(seed)
    x = 0.01 * rng.standard_normal(int(fs * seconds)).astype(np.float32)
    for t in times:
        for at, gain in [(t, 1.0)] + ([(t + 0.12, 0.3)] if echo else []):
            i, n = int(at * fs), int(0.01 * fs)
            burst = rng.standard_normal(n) * np.exp(-np.arange(n) / (0.002 * fs))
            x[i : i + n] += (gain * burst).astype(np.float32)
    return x


def test_claps_are_heard_at_the_right_time():
    for fs in (48000, 44100, 16000):
        est = clap.onset_times(_clicks(fs, 10, [1.234, 3.5, 6.78]), fs)
        assert np.allclose(est, [1.234, 3.5, 6.78], atol=0.002), fs


def test_an_echo_is_not_a_second_clap():
    est = clap.onset_times(_clicks(48000, 5, [1.0, 3.0], echo=True), 48000)
    assert len(est) == 2


def test_speech_is_not_taken_for_claps():
    fs = 16000
    t = np.arange(fs * 6) / fs
    rng = np.random.default_rng(1)
    # Voiced sound: harmonics of 150 Hz, rising and falling in syllables.
    voice = sum(np.sin(2 * np.pi * 150 * k * t) / k for k in range(1, 12))
    syllables = 0.5 * (1 + np.sin(2 * np.pi * 4 * t)) ** 2
    x = (0.1 * voice * syllables + 0.005 * rng.standard_normal(t.size)).astype(np.float32)
    assert len(clap.onset_times(x, fs)) == 0


def test_hands_meeting_are_seen_between_frames():
    t = np.arange(100) * 40.0  # 25 frames per second
    gap = np.full(100, 0.6)
    # The hands close and open around 2010 ms: frames 49-51 at 0.3, 0.02, 0.1.
    gap[48:53] = [0.5, 0.3, 0.02, 0.1, 0.4]
    seen = clap.seen_claps(t, gap)
    assert len(seen) == 1 and abs(seen[0] - 2000.0) < 20.0
    # Hands already together, never apart before: no clap.
    assert len(clap.seen_claps(t, np.full(100, 0.05))) == 0


def test_pairs_and_verdicts():
    pairs = clap.match([1030.0, 5000.0, 9999.0], [1000.0, 5010.0])
    assert [round(p[2]) for p in pairs] == [30, -10]
    assert clap.verdict([10.0, 12.0], 40.0) == "ok"
    assert clap.verdict([30.0], 40.0) == "check"
    assert clap.verdict([60.0], 40.0) == "off"
    assert clap.verdict([], 40.0) == "none"


def test_runner_measures_a_known_offset(tmp_path):
    from scipy.io import wavfile

    s = tmp_path / "sess"
    (s / "audio").mkdir(parents=True)
    (s / "video").mkdir()
    (s / "pose").mkdir()
    start_ns = 5_000_000_000
    (s / "session_meta.json").write_text(json.dumps({"session_start_elapsed_ns": start_ns}))
    (s / "video" / "video_1.mp4").write_bytes(b"")
    # Claps at 2, 5 and 8 s of video; the audio has them 30 ms late.
    claps_s = [2.0, 5.0, 8.0]
    fs = 48000
    x = _clicks(fs, 10, [c + 0.030 for c in claps_s])
    wavfile.write(s / "audio" / "audio.wav", fs, (x * 32767).astype(np.int16))
    frames = []
    for k in range(250):  # 25 fps, 10 s
        t = k * 0.04
        near = min(abs(t - c) for c in claps_s)
        gap = 0.02 if near < 0.02 else 0.3 if near < 0.1 else 0.6
        kp = np.zeros((17, 2))
        kp[10, 0] = gap * 300.0  # right wrist this far from the left, box 300 px tall
        frames.append(
            {
                "timestamp_ns": start_ns + int(t * 1e9),
                "subjects": [
                    {
                        "bbox_xyxy": [0, 0, 100, 300],
                        "keypoints": kp.tolist(),
                        "visibilities": [1.0] * 17,
                    }
                ],
            }
        )
    (s / "pose" / "video_1.n.pose.json").write_text(json.dumps({"frames": frames}))
    assert run_clap_check.main(["--session", str(s)]) == 0
    doc = json.loads((s / "sync_check" / "clap_check.json").read_text())
    mic = doc["cameras"][0]["microphones"][0]
    assert mic["matched"] == 3 and mic["clock"] == "session_start"
    assert mic["median_offset_ms"] == pytest.approx(30.0, abs=5.0)
    assert mic["verdict"] == "check" and doc["verdict"] == "check"  # over half a frame (20 ms)


def test_runner_says_to_run_pose(tmp_path, capsys):
    from scipy.io import wavfile

    s = tmp_path / "sess"
    (s / "audio").mkdir(parents=True)
    (s / "video").mkdir()
    (s / "session_meta.json").write_text(json.dumps({"session_start_elapsed_ns": 0}))
    (s / "video" / "video_0.mp4").write_bytes(b"")
    wavfile.write(s / "audio" / "audio.wav", 16000, np.zeros(16000, np.int16))
    assert run_clap_check.main(["--session", str(s)]) == 1
    assert "run Pose first" in capsys.readouterr().err


def test_audio_a_whole_second_off_is_found_not_paired_with_the_next_clap():
    # Claps seen at 2, 3 and 5 s (clap, 1 s, clap, 2 s, clap); heard 1 s late.
    seen = np.array([2000.0, 3000.0, 5000.0])
    heard = seen + 1000.0
    assert clap.estimate_lag(heard, seen) == pytest.approx(1000.0)
    pairs = clap.match(heard, seen)
    assert [round(p[2]) for p in pairs] == [1000, 1000, 1000]
    assert clap.verdict([p[2] for p in pairs], 40.0) == "off"
    # Even evenly spaced claps: the offset all three agree on wins over the
    # neighbours' 0 that only two agree on.
    even = np.array([2000.0, 3000.0, 4000.0])
    assert clap.estimate_lag(even + 1000.0, even) == pytest.approx(1000.0)
    # A sharp sound nobody saw (a door) does not move it.
    assert clap.estimate_lag(np.append(heard, 4200.0), seen) == pytest.approx(1000.0)
