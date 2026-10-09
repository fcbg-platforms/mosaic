"""
End-to-end test of run_conversation.py on a small synthetic session: two
voices taking turns, the on-camera person's mouth moving with their own
speech, an audio timing file with clock drift, and a diarized transcript.
"""

import csv
import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_conversation as rc
from conversation import vad

RATE = 16000
FPS = 50.0
SESSION_START_NS = 5_000_000_000
AUDIO_START_NS = SESSION_START_NS + 40_000_000  # audio really starts 40 ms later
VIDEO_START_NS = SESSION_START_NS + 120_000_000

# (speaker, start_s, end_s) in audio time. "S" is on camera.
SCRIPT = [("I", 1.0, 4.0), ("S", 4.5, 9.0), ("I", 8.6, 11.0), ("S", 11.8, 15.0)]


def _voice(n, f0, rng):
    t = np.arange(n) / RATE
    v = sum(np.sin(2 * np.pi * f0 * k * t) / k for k in range(1, 8))
    return 0.2 * v * (0.6 + 0.4 * np.sin(2 * np.pi * 4 * t)) + rng.normal(0, 0.002, n)


def _make_session(root: Path, diarized: bool) -> None:
    import cv2
    from scipy.io import wavfile

    rng = np.random.default_rng(0)
    total_s = 17.0
    x = rng.normal(0, 0.002, int(total_s * RATE))
    for who, a, b in SCRIPT:
        i, j = int(a * RATE), int(b * RATE)
        x[i:j] += _voice(j - i, 120.0 if who == "S" else 210.0, rng)
    (root / "audio").mkdir(parents=True)
    (root / "video").mkdir()
    (root / "face_dynamics").mkdir()
    wavfile.write(root / "audio" / "audio.wav", RATE, (np.clip(x, -1, 1) * 32767).astype(np.int16))
    ns_per_sample = 1e9 / RATE * (1 + 50e-6)
    with (root / "audio" / "audio.timing.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["sample_count", "elapsed_ns"])
        for k in range(1600, len(x), 1600):
            w.writerow([k, int(AUDIO_START_NS + k * ns_per_sample + 1e6 + rng.exponential(2e6))])
    (root / "session_meta.json").write_text(
        json.dumps({"session_start_elapsed_ns": SESSION_START_NS})
    )
    if diarized:
        segs = [
            {
                "start_ms": int(a * 1000),
                "end_ms": int(b * 1000),
                "speaker": f"SPK_{who}",
                "text": "one two three",
            }
            for who, a, b in SCRIPT
        ]
        (root / "audio" / "audio.transcript.json").write_text(
            json.dumps({"diarization": True, "segments": segs})
        )
    n_frames = int((total_s - 0.5) * FPS)
    ft = VIDEO_START_NS + np.arange(n_frames) * 1e9 / FPS
    ta = (ft - AUDIO_START_NS) / ns_per_sample / RATE  # frame time in audio seconds
    mouth = 0.05 + rng.normal(0, 0.005, n_frames)
    for who, a, b in SCRIPT:
        if who == "S":
            sel = (ta >= a) & (ta < b)
            mouth[sel] += 0.25 * np.abs(np.sin(2 * np.pi * 2.5 * ta[sel]))
    writer = cv2.VideoWriter(
        str(root / "video" / "video_0.mp4"), cv2.VideoWriter_fourcc(*"mp4v"), FPS, (64, 48)
    )
    for _ in range(n_frames):
        writer.write(np.zeros((48, 64, 3), np.uint8))
    writer.release()
    with (root / "video" / "timestamps_cam0.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["frame_id", "elapsed_ns", "wall_ns", "hw_timestamp_ns"])
        for i, t in enumerate(ft):
            w.writerow([i + 1, int(t), 0, 0])
    with (root / "face_dynamics" / "video_0.face_dynamics.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["frame_index", "timestamp_ms", "face_detected", "jawOpen"])
        for i, t in enumerate(ft):
            w.writerow([i, round(t / 1e6, 3), 1, f"{mouth[i]:.4f}"])


@pytest.fixture
def energy_vad(monkeypatch):
    """Silero is trained on real voices; the synthetic ones use the energy
    detector."""
    monkeypatch.setattr(vad, "detect_speech_silero", lambda *a, **k: None)


@pytest.mark.parametrize("diarized", [False, True])
def test_turns_and_response_times_end_to_end(tmp_path, energy_vad, diarized):
    _make_session(tmp_path, diarized)
    assert rc.main(["--session", str(tmp_path), "--no-video"]) == 0
    doc = json.loads((tmp_path / "conversation" / "video_0.conversation.json").read_text())
    assert doc["schema"] == rc.SCHEMA
    assert doc["timing"]["method"] == "timing_file"
    assert doc["timing"]["audio_start_ms"] == pytest.approx(AUDIO_START_NS / 1e6, abs=3)
    assert doc["attribution"]["method"] == ("diarization" if diarized else "mouth")
    assert [t["speaker"] for t in doc["turns"]] == ["other", "subject", "other", "subject"]
    ftos = [x["fto_ms"] for x in doc["transitions"]]
    # Truth: +500, -400 (the interviewer starts before the subject stops), +800.
    assert ftos[0] == pytest.approx(500, abs=80)
    assert ftos[2] == pytest.approx(800, abs=80)
    if diarized:
        assert ftos[1] == pytest.approx(-400, abs=80)  # the overlap is seen
        assert doc["summary"]["speakers"]["subject"]["words"] == 6
        assert doc["turns"][1]["text"] == "one two three"
    else:
        assert -450 <= ftos[1] <= 80  # mouth alone cannot see the overlap
    # Times are on the video's clock: the first turn starts 1 s into the audio.
    assert doc["turns"][0]["start_ms"] == pytest.approx(AUDIO_START_NS / 1e6 + 1000, abs=60)
    rows = list(csv.DictReader((tmp_path / "conversation" / "video_0.turns.csv").open()))
    assert len(rows) == 4 and rows[1]["speaker"] == "subject"
    assert len(doc["signals"]["audio_db"]) > 0


def test_a_session_without_audio_fails_cleanly(tmp_path):
    (tmp_path / "audio").mkdir()
    assert rc.main(["--session", str(tmp_path)]) == 1
