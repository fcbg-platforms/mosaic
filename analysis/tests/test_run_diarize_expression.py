"""
run_diarize.py and run_expression.py around their models: what they write,
and what happens when a model step fails, with stand-in models (no
faster-whisper, pyannote or mediapipe needed). The models' own logic is
covered by test_diarize_pipeline.py and the expression tests.
"""

import json
import sys
from pathlib import Path

import cv2
import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_diarize
import run_expression
from diarize.pipeline import DIARIZATION_STATUSES
from expression.detector import BLENDSHAPE_NAMES, FaceExpression

# ── Speaker diarization ─────────────────────────────────────────────────────

WHISPER = [
    {"start": 0.0, "end": 2.0, "text": "Hello there."},
    {"start": 2.5, "end": 4.0, "text": "Hi."},
]
TURNS = [
    {"start": 0.0, "end": 2.2, "speaker": "SPEAKER_00"},
    {"start": 2.3, "end": 4.5, "speaker": "SPEAKER_01"},
]


@pytest.fixture
def fake_models(monkeypatch):
    calls = {"transcribe": [], "diarize": []}

    def transcribe(model, path, language):
        calls["transcribe"].append(path.name)
        if "corrupt" in path.name:
            raise RuntimeError("not a WAV file")
        return [dict(s) for s in WHISPER], "en"

    def diarize(pipeline, path, lo, hi):
        calls["diarize"].append((path.name, lo, hi))
        if pipeline == "broken":
            raise RuntimeError("401 gated model")
        return [dict(t) for t in TURNS]

    monkeypatch.setattr(run_diarize, "transcribe_audio", transcribe)
    monkeypatch.setattr(run_diarize, "diarize_audio", diarize)
    return calls


def run_audio(tmp_path, pipeline, load=("ok", ""), name="audio.wav"):
    audio = tmp_path / name
    audio.write_bytes(b"RIFF")
    out = tmp_path / "audio.transcript.json"
    ok = run_diarize.process_audio(audio, out, "small", object(), pipeline, None, 1, 2, load)
    return ok, out


def test_transcript_with_speakers(tmp_path, fake_models):
    ok, out = run_audio(tmp_path, "pipeline")
    assert ok
    doc = json.loads(out.read_text(encoding="utf-8"))
    assert doc["source_audio"] == "audio.wav"
    assert doc["model"] == "small" and doc["language"] == "en"
    assert doc["diarization"] is True and doc["diarization_status"] == "ok"
    assert [s["speaker"] for s in doc["segments"]] == ["SPEAKER_00", "SPEAKER_01"]
    assert fake_models["diarize"] == [("audio.wav", 1, 2)]


def test_a_failed_diarization_still_writes_the_transcript_and_says_why(tmp_path, fake_models):
    ok, out = run_audio(tmp_path, "broken")
    assert ok
    doc = json.loads(out.read_text(encoding="utf-8"))
    assert doc["diarization"] is False
    assert doc["diarization_status"] == "run_failed"
    assert "401" in doc["diarization_detail"]
    assert [s["text"] for s in doc["segments"]] == ["Hello there.", "Hi."]
    assert all(not s["speaker"] for s in doc["segments"])


def test_no_pipeline_is_transcript_only_with_the_load_reason(tmp_path, fake_models):
    ok, out = run_audio(tmp_path, None, load=("no_token", "No Hugging Face token."))
    assert ok
    doc = json.loads(out.read_text(encoding="utf-8"))
    assert doc["diarization_status"] == "no_token"
    assert doc["diarization_status"] in DIARIZATION_STATUSES
    assert fake_models["diarize"] == []


def test_unreadable_audio_fails_and_writes_nothing(tmp_path, fake_models):
    ok, out = run_audio(tmp_path, "pipeline", name="corrupt.wav")
    assert not ok
    assert not out.exists()


def test_session_writes_each_microphone_and_exits_on_any_failure(
    tmp_path, fake_models, monkeypatch
):
    audio = tmp_path / "audio"
    audio.mkdir()
    (audio / "audio_0.wav").write_bytes(b"RIFF")
    (audio / "corrupt_1.wav").write_bytes(b"RIFF")
    loads = []
    monkeypatch.setattr(
        run_diarize,
        "_load_models",
        lambda *a: loads.append(a) or ("cpu", object(), "pipeline", ("ok", "")),
    )
    with pytest.raises(SystemExit) as e:
        run_diarize.process_session(tmp_path, "small", None, None, None, 1, 4, False)
    assert e.value.code == 1
    assert len(loads) == 1  # models loaded once for the whole session
    assert (audio / "audio_0.transcript.json").exists()
    assert not (audio / "corrupt_1.transcript.json").exists()


# ── Facial expression ───────────────────────────────────────────────────────


class FakeDetector:
    """Two faces in odd frames, none in even ones."""

    def __init__(self, *a, **kw):
        self.frames = 0

    def detect(self, frame):
        self.frames += 1
        if self.frames % 2:
            return []
        smile = [0.0] * len(BLENDSHAPE_NAMES)
        for name in ("mouthSmileLeft", "mouthSmileRight"):
            smile[BLENDSHAPE_NAMES.index(name)] = 0.9
        return [
            FaceExpression((10.0, 10.0, 50.0, 60.0), 1.0, smile),
            FaceExpression((80.0, 10.0, 120.0, 60.0), 1.0, [0.0] * len(BLENDSHAPE_NAMES)),
        ]


def write_mp4(path: Path, frames: int) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    out = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*"mp4v"), 10.0, (160, 120))
    assert out.isOpened()
    for _ in range(frames):
        out.write(np.full((120, 160, 3), 100, np.uint8))
    out.release()
    return path


def test_expression_session_output(tmp_path, monkeypatch):
    session = tmp_path / "session"
    write_mp4(session / "video" / "video_3.mp4", 4)
    (session / "video" / "timestamps_cam3.csv").write_text(
        "frame_id,elapsed_ns,wall_ns,hw_timestamp_ns\n"
        + "".join(f"{i + 1},{1000 + i},0,0\n" for i in range(4))
    )
    monkeypatch.setattr(run_expression, "MediaPipeExpressionDetector", FakeDetector)

    run_expression.process_session(session, "heuristic", 5, 0.5, 1)

    doc = json.loads((session / "expression" / "video_3.expression.json").read_text())
    assert doc["backend"] == "heuristic"
    assert doc["blendshape_names"] == BLENDSHAPE_NAMES
    assert "au_names" not in doc
    frames = doc["frames"]
    assert [f["timestamp_ns"] for f in frames] == [1000, 1001, 1002, 1003]
    assert {f["camera_index"] for f in frames} == {3}
    assert [len(f["subjects"]) for f in frames] == [0, 2, 0, 2]
    a, b = frames[1]["subjects"]
    assert [a["subject_id"], b["subject_id"]] == [0, 1]
    assert a["dominant_expression"] == "Happy"
    assert "action_units" not in a  # only the py-feat backend adds AUs
    assert len(a["blendshape_scores"]) == len(BLENDSHAPE_NAMES)


def test_expression_skip_and_unknown_backend(tmp_path):
    video = write_mp4(tmp_path / "video_0.mp4", 7)
    classify = run_expression._make_classify_fn("heuristic")
    run_expression.process_video(video, FakeDetector(), classify, "heuristic", skip=3)
    doc = json.loads((tmp_path / "video_0.expression.json").read_text())
    assert [f["frame_index"] for f in doc["frames"]] == [0, 3, 6]
    with pytest.raises(ValueError):
        run_expression._make_classify_fn("nonsense")
