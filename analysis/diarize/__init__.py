"""Transcription + speaker-diarization pipeline for MOSAIC's Speaker
Diarization analysis plugin."""

from .pipeline import (
    DiarizationTurn,
    TranscriptSegment,
    WhisperSegment,
    assign_speakers,
    diarize_audio,
    load_diarization_pipeline,
    load_whisper_model,
    resolve_device,
    transcribe_audio,
)

__all__ = [
    "WhisperSegment",
    "DiarizationTurn",
    "TranscriptSegment",
    "resolve_device",
    "load_whisper_model",
    "transcribe_audio",
    "load_diarization_pipeline",
    "diarize_audio",
    "assign_speakers",
]
