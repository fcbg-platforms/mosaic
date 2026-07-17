"""
Pure-logic tests for diarize/pipeline.py's assign_speakers() and
resolve_device() — no torch/faster-whisper/pyannote import required, since
both functions have zero dependencies beyond the standard library.

assign_speakers() is the highest-value function to get right in this
feature: a bug here silently mislabels who said what in the exported
transcript, the same severity class as facemask's expand_and_clip().
"""
import sys
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parent.parent))

from diarize.pipeline import assign_speakers, resolve_device


def test_assign_speakers_segment_fully_inside_one_turn():
    whisper = [{"start": 1.0, "end": 2.0, "text": "hello"}]
    turns = [{"start": 0.0, "end": 5.0, "speaker": "SPEAKER_00"}]
    result = assign_speakers(whisper, turns)
    assert result == [{"start_ms": 1000, "end_ms": 2000, "speaker": "SPEAKER_00", "text": "hello"}]


def test_assign_speakers_segment_spanning_two_turns_picks_larger_overlap():
    # Segment [0, 10] overlaps SPEAKER_00 by 3s ([0,3]) and SPEAKER_01 by 7s ([3,10]).
    whisper = [{"start": 0.0, "end": 10.0, "text": "mixed"}]
    turns = [
        {"start": 0.0, "end": 3.0, "speaker": "SPEAKER_00"},
        {"start": 3.0, "end": 10.0, "speaker": "SPEAKER_01"},
    ]
    result = assign_speakers(whisper, turns)
    assert result[0]["speaker"] == "SPEAKER_01"


def test_assign_speakers_zero_overlap_yields_none_speaker():
    whisper = [{"start": 0.0, "end": 1.0, "text": "early"}]
    turns = [{"start": 5.0, "end": 6.0, "speaker": "SPEAKER_00"}]
    result = assign_speakers(whisper, turns)
    assert result[0]["speaker"] is None


def test_assign_speakers_empty_turns_yields_none_speaker_for_every_segment():
    whisper = [
        {"start": 0.0, "end": 1.0, "text": "a"},
        {"start": 1.0, "end": 2.0, "text": "b"},
    ]
    result = assign_speakers(whisper, [])
    assert all(seg["speaker"] is None for seg in result)


def test_assign_speakers_segment_exactly_on_turn_boundary_has_no_overlap():
    # [0,1] and [1,2] touch at a point but share zero duration — no overlap.
    whisper = [{"start": 1.0, "end": 2.0, "text": "boundary"}]
    turns = [{"start": 0.0, "end": 1.0, "speaker": "SPEAKER_00"}]
    result = assign_speakers(whisper, turns)
    assert result[0]["speaker"] is None


def test_assign_speakers_rounds_seconds_to_milliseconds():
    whisper = [{"start": 0.1234, "end": 1.9876, "text": "x"}]
    result = assign_speakers(whisper, [])
    assert result[0]["start_ms"] == 123
    assert result[0]["end_ms"] == 1988


def test_resolve_device_returns_explicit_arg_unchanged():
    assert resolve_device("cpu") == "cpu"
    assert resolve_device("cuda") == "cuda"


def test_resolve_device_auto_detects_cuda_available():
    with patch("torch.cuda.is_available", return_value=True):
        assert resolve_device(None) == "cuda"


def test_resolve_device_auto_falls_back_to_cpu():
    with patch("torch.cuda.is_available", return_value=False):
        assert resolve_device(None) == "cpu"
