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

import pytest
from diarize.pipeline import (
    DIARIZATION_STATUSES,
    assign_speakers,
    resolve_device,
    resolve_diarization_status,
)


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


# ── resolve_diarization_status ────────────────────────────────────────────────
#
# The point of these: before this function existed, "no Hugging Face token",
# "the gated model refused to load" and "it ran and found nobody" all produced
# byte-identical output — a transcript with every speaker null — and the reason
# was printed to a log that had scrolled away by the time anyone looked.


def test_status_ok_when_everything_worked():
    status, detail = resolve_diarization_status(
        load_status="ok", load_detail="", run_error=None, turn_count=3, labeled_segment_count=7
    )
    assert status == "ok"
    assert detail == ""


def test_load_failure_reason_survives_verbatim():
    # The message matters: a 401 here means the token exists but its owner
    # never accepted the gated model's terms, which is the single most likely
    # reason a correct-looking token still yields no speakers.
    status, detail = resolve_diarization_status(
        load_status="load_failed",
        load_detail="401 Client Error: Gated repo",
        run_error=None,
        turn_count=0,
        labeled_segment_count=0,
    )
    assert status == "load_failed"
    assert "401" in detail


@pytest.mark.parametrize("load_status", ["skipped_by_user", "no_token", "load_failed"])
def test_load_status_takes_precedence_over_everything_downstream(load_status):
    # No pipeline means no turns, so the downstream counts are trivially zero.
    # They must not be allowed to relabel the real cause as "no_turns".
    status, _ = resolve_diarization_status(
        load_status=load_status,
        load_detail="because",
        run_error=None,
        turn_count=0,
        labeled_segment_count=0,
    )
    assert status == load_status


def test_run_error_beats_empty_turns():
    status, detail = resolve_diarization_status(
        load_status="ok",
        load_detail="",
        run_error="CUDA out of memory",
        turn_count=0,
        labeled_segment_count=0,
    )
    assert status == "run_failed"
    assert "CUDA" in detail


def test_no_turns_when_model_heard_nobody():
    status, detail = resolve_diarization_status(
        load_status="ok", load_detail="", run_error=None, turn_count=0, labeled_segment_count=0
    )
    assert status == "no_turns"
    assert detail


def test_turns_that_match_nothing_get_their_own_status():
    # Must not collapse into no_turns: the UI states "found no speaker turns"
    # for that one, which would directly contradict this detail line, and the
    # advice ("check the mic captured speech") would be wrong — it did.
    status, detail = resolve_diarization_status(
        load_status="ok", load_detail="", run_error=None, turn_count=4, labeled_segment_count=0
    )
    assert status == "no_overlap"
    assert "4 speaker turn(s)" in detail


def test_every_returned_status_is_declared():
    # DIARIZATION_STATUSES is mirrored by the C++ enum; a value returned here
    # but missing from the tuple would silently become Unknown in the UI.
    cases = [
        ("ok", "", None, 1, 1),
        ("skipped_by_user", "x", None, 0, 0),
        ("no_token", "x", None, 0, 0),
        ("load_failed", "x", None, 0, 0),
        ("ok", "", "boom", 0, 0),
        ("ok", "", None, 0, 0),
        ("ok", "", None, 2, 0),
    ]
    for load_status, load_detail, run_error, turns, labeled in cases:
        status, _ = resolve_diarization_status(
            load_status=load_status,
            load_detail=load_detail,
            run_error=run_error,
            turn_count=turns,
            labeled_segment_count=labeled,
        )
        assert status in DIARIZATION_STATUSES
