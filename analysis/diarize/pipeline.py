"""
Transcription + speaker-diarization pipeline for MOSAIC recorded audio.

Two independent stages combined via the standard WhisperX-style recipe:
    1. transcribe_audio()  — faster-whisper, always run.
    2. diarize_audio()     — pyannote.audio, run only if a Hugging Face
       token is available and the caller hasn't opted out.
    3. assign_speakers()   — pure interval-overlap matching that labels
       each transcribed segment with whichever diarization turn covers it
       the most. This is the one function in this module with zero
       dependencies beyond the standard library, so it can be unit-tested
       without installing torch/faster-whisper/pyannote at all — see
       analysis/tests/test_diarize_pipeline.py.

faster-whisper and pyannote.audio are imported lazily (inside the
functions that need them) rather than at module level, for the same
reason: importing this module (e.g. to reach assign_speakers() in a test)
should not require the heavy ML stack to be installed.
"""
from __future__ import annotations

from pathlib import Path
from typing import Optional, TypedDict


class WhisperSegment(TypedDict):
    start: float   # seconds
    end: float     # seconds
    text: str


class DiarizationTurn(TypedDict):
    start: float   # seconds
    end: float     # seconds
    speaker: str


class TranscriptSegment(TypedDict):
    start_ms: int
    end_ms: int
    speaker: Optional[str]   # None = no diarization turn overlapped (or none was run)
    text: str


# ── Device selection ──────────────────────────────────────────────────────────

def resolve_device(device_arg: Optional[str]) -> str:
    """Returns device_arg unchanged if given, else "cuda" if a CUDA device is
    available, else "cpu". Shared by transcribe_audio()/diarize_audio() so
    they never disagree about which hardware to use for the same run."""
    if device_arg:
        return device_arg
    import torch
    return "cuda" if torch.cuda.is_available() else "cpu"


# ── Transcription (faster-whisper) ────────────────────────────────────────────

def load_whisper_model(model_size: str, device: str):
    """Loads a faster-whisper model once, for reuse across every audio file
    in a session — transcribe_audio() used to build a fresh model per call,
    which reloaded weights from disk/cache once per microphone instead of
    once per run."""
    from faster_whisper import WhisperModel

    compute_type = "float16" if device == "cuda" else "int8"
    return WhisperModel(model_size, device=device, compute_type=compute_type)


def transcribe_audio(model, audio_path: Path,
                      language: Optional[str]) -> tuple[list[WhisperSegment], str]:
    """Returns (segments, detected_language) for a model built by
    load_whisper_model(). faster-whisper decodes and resamples the input file
    internally (mono/16kHz) — no separate preprocessing step is needed
    regardless of the recording's original sample rate/channel count."""
    segments_iter, info = model.transcribe(str(audio_path), language=language or None)
    segments: list[WhisperSegment] = [
        {"start": seg.start, "end": seg.end, "text": seg.text.strip()}
        for seg in segments_iter
    ]
    return segments, info.language


# ── Diarization (pyannote.audio) ──────────────────────────────────────────────

_GATING_HELP = (
    "pyannote's diarization models are gated on Hugging Face. To use one: "
    "(1) create a free account at https://huggingface.co, (2) accept the "
    "terms of use for https://huggingface.co/pyannote/speaker-diarization-3.1 "
    "and https://huggingface.co/pyannote/segmentation-3.0, (3) generate an "
    "access token at https://huggingface.co/settings/tokens and paste it into "
    "the Diarization plugin's token field."
)


def load_diarization_pipeline(hf_token: str, device: str):
    """Loads the pyannote diarization pipeline once, for reuse across every
    audio file in a session — diarize_audio() used to build a fresh pipeline
    per call, which reloaded weights from disk/cache once per microphone
    instead of once per run. Raises RuntimeError with actionable setup
    instructions if the token is missing/invalid or the gated models haven't
    been accepted yet, rather than letting a raw HTTP/auth exception
    propagate to the caller's log."""
    import torch
    from pyannote.audio import Pipeline

    try:
        pipeline = Pipeline.from_pretrained(
            "pyannote/speaker-diarization-3.1", use_auth_token=hf_token)
    except Exception as exc:  # noqa: BLE001 - re-raised with actionable context below
        raise RuntimeError(
            f"Could not load the pyannote diarization pipeline ({exc}). {_GATING_HELP}"
        ) from exc

    pipeline.to(torch.device(device))
    return pipeline


def diarize_audio(pipeline, audio_path: Path,
                   min_speakers: int, max_speakers: int) -> list[DiarizationTurn]:
    """Runs a pipeline built by load_diarization_pipeline() against one audio
    file."""
    kwargs = {}
    if min_speakers > 0:
        kwargs["min_speakers"] = min_speakers
    if max_speakers > 0:
        kwargs["max_speakers"] = max_speakers

    diarization = pipeline(str(audio_path), **kwargs)

    turns: list[DiarizationTurn] = []
    for turn, _, speaker in diarization.itertracks(yield_label=True):
        turns.append({"start": turn.start, "end": turn.end, "speaker": speaker})
    return turns


# ── Speaker assignment (pure, no I/O) ─────────────────────────────────────────

def assign_speakers(whisper_segments: list[WhisperSegment],
                     diarization_turns: list[DiarizationTurn]) -> list[TranscriptSegment]:
    """Assigns each whisper segment the speaker of whichever diarization turn
    overlaps it the most (by duration), the standard WhisperX-style recipe.
    A segment with zero overlap against every turn (or when diarization_turns
    is empty, e.g. diarization was skipped) gets speaker=None rather than a
    guessed label."""
    result: list[TranscriptSegment] = []
    for seg in whisper_segments:
        best_speaker: Optional[str] = None
        best_overlap = 0.0
        for turn in diarization_turns:
            overlap = min(seg["end"], turn["end"]) - max(seg["start"], turn["start"])
            if overlap > best_overlap:
                best_overlap = overlap
                best_speaker = turn["speaker"]
        result.append({
            "start_ms": round(seg["start"] * 1000),
            "end_ms":   round(seg["end"] * 1000),
            "speaker":  best_speaker,
            "text":     seg["text"],
        })
    return result
