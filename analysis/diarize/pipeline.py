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
    """One faster-whisper transcription segment.

    Attributes
    ----------
    start, end : float
        Segment bounds, in seconds.
    text : str
        Transcribed text for this segment.
    """
    start: float   # seconds
    end: float     # seconds
    text: str


class DiarizationTurn(TypedDict):
    """One pyannote.audio speaker turn.

    Attributes
    ----------
    start, end : float
        Turn bounds, in seconds.
    speaker : str
        Speaker label (e.g. ``"SPEAKER_00"``).
    """
    start: float   # seconds
    end: float     # seconds
    speaker: str


class TranscriptSegment(TypedDict):
    """One final, speaker-labeled transcript segment (:func:`assign_speakers`'s output).

    Attributes
    ----------
    start_ms, end_ms : int
        Segment bounds, in milliseconds.
    speaker : str or None
        Assigned speaker label, or ``None`` if no diarization turn
        overlapped this segment (or diarization wasn't run at all).
    text : str
        Transcribed text for this segment.
    """
    start_ms: int
    end_ms: int
    speaker: Optional[str]   # None = no diarization turn overlapped (or none was run)
    text: str


# ── Device selection ──────────────────────────────────────────────────────────

def resolve_device(device_arg: Optional[str]) -> str:
    """Resolve which device to run inference on.

    Parameters
    ----------
    device_arg : str or None
        Explicit device (e.g. ``"cuda"``, ``"cpu"``), or ``None`` to
        auto-detect.

    Returns
    -------
    str
        ``device_arg`` unchanged if given, else ``"cuda"`` if a CUDA
        device is available, else ``"cpu"``.

    Notes
    -----
    Shared by :func:`load_whisper_model`/:func:`load_diarization_pipeline`
    so they never disagree about which hardware to use for the same run.
    """
    if device_arg:
        return device_arg
    import torch
    return "cuda" if torch.cuda.is_available() else "cpu"


# ── Transcription (faster-whisper) ────────────────────────────────────────────

def load_whisper_model(model_size: str, device: str):
    """Load a faster-whisper model once, for reuse across a whole session.

    Parameters
    ----------
    model_size : str
        faster-whisper model size (e.g. ``"small"``, ``"large-v3"``).
    device : str
        Inference device, typically from :func:`resolve_device`.

    Returns
    -------
    faster_whisper.WhisperModel
        A loaded model, ready for repeated :func:`transcribe_audio` calls.

    Notes
    -----
    Call this once per run and pass the result to every
    :func:`transcribe_audio` call in that run — building a fresh model per
    audio file reloads weights from disk/cache once per microphone instead
    of once per run.
    """
    from faster_whisper import WhisperModel

    compute_type = "float16" if device == "cuda" else "int8"
    return WhisperModel(model_size, device=device, compute_type=compute_type)


def transcribe_audio(model, audio_path: Path,
                      language: Optional[str]) -> tuple[list[WhisperSegment], str]:
    """Transcribe one audio file with a pre-loaded whisper model.

    Parameters
    ----------
    model : faster_whisper.WhisperModel
        A model from :func:`load_whisper_model`.
    audio_path : pathlib.Path
        Path to the audio file. faster-whisper decodes/resamples it
        internally (mono/16kHz) — no separate preprocessing step is
        needed regardless of the recording's original sample
        rate/channel count.
    language : str or None
        Force a language code (e.g. ``"en"``), or ``None`` to auto-detect.

    Returns
    -------
    tuple of (list of WhisperSegment, str)
        ``(segments, detected_language)``.
    """
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
    "terms of use for https://huggingface.co/pyannote/speaker-diarization-community-1, "
    "(3) generate an access token at https://huggingface.co/settings/tokens and "
    "paste it into the Diarization plugin's token field."
)


def load_diarization_pipeline(hf_token: str, device: str):
    """Load the pyannote diarization pipeline once, for reuse across a session.

    Parameters
    ----------
    hf_token : str
        Hugging Face access token with the gated diarization models
        accepted (see the module docstring's gating instructions).
    device : str
        Inference device, typically from :func:`resolve_device`.

    Returns
    -------
    pyannote.audio.Pipeline
        A loaded pipeline, ready for repeated :func:`diarize_audio` calls.

    Raises
    ------
    RuntimeError
        If the token is missing/invalid or the gated models haven't been
        accepted yet — raised with actionable setup instructions rather
        than letting a raw HTTP/auth exception propagate to the caller's
        log.

    Notes
    -----
    Call this once per run and pass the result to every
    :func:`diarize_audio` call in that run — building a fresh pipeline per
    audio file reloads weights from disk/cache once per microphone
    instead of once per run.
    """
    import torch
    from pyannote.audio import Pipeline

    try:
        # "community-1" (not the older "speaker-diarization-3.1") — the
        # pipeline pyannote.audio 4.x itself recommends: better speaker-
        # assignment/counting accuracy, and avoids a confusing 2-hop gating
        # chain (this installed pyannote.audio version's own "3.1" wrapper
        # transitively pulls a community-1 component anyway, which would
        # otherwise fail with an unrelated-looking GatedRepoError). The auth
        # kwarg is `token=`, not the older `use_auth_token=` — that name was
        # removed in pyannote.audio 4.x (confirmed via
        # inspect.signature(Pipeline.from_pretrained) against the installed
        # version), so a still-`use_auth_token=`-shaped call throws
        # TypeError immediately, never reaching the network at all.
        pipeline = Pipeline.from_pretrained(
            "pyannote/speaker-diarization-community-1", token=hf_token)
    except Exception as exc:  # noqa: BLE001 - re-raised with actionable context below
        raise RuntimeError(
            f"Could not load the pyannote diarization pipeline ({exc}). {_GATING_HELP}"
        ) from exc

    pipeline.to(torch.device(device))
    return pipeline


def _load_waveform_dict(audio_path: Path) -> dict:
    """Pre-decodes a WAV file into pyannote's ``{'waveform': (channel,
    time) torch.Tensor, 'sample_rate': int}`` escape-hatch format.

    pyannote.audio 4.x's own audio-loading path (handing it a raw file
    path) goes through ``torchcodec``, which is confirmed broken on this
    machine — its Windows DLLs fail to load against every FFmpeg build it
    tries (4 through 8), turning into ``RuntimeError: torchcodec is not
    available`` at diarize time regardless of the pipeline itself loading
    fine. ``scipy.io.wavfile`` has zero torchcodec/ffmpeg dependency and
    Mosaic's recordings are always plain PCM WAV (see docs/recording.rst),
    so pre-decoding ourselves is a safe, always-available bypass rather
    than a narrow special case — verified directly against a real session
    recording before landing this.
    """
    import numpy as np
    import torch
    from scipy.io import wavfile

    sample_rate, samples = wavfile.read(str(audio_path))
    if samples.ndim == 1:
        samples = samples[:, None]   # mono -> (time, 1)
    if np.issubdtype(samples.dtype, np.integer):
        # e.g. int16 -> 32768.0, matching this project's established PCM
        # normalization convention (src/audio/audio_envelope.cpp).
        max_val = float(2 ** (samples.dtype.itemsize * 8 - 1))
        samples = samples.astype(np.float32) / max_val
    waveform = torch.from_numpy(np.ascontiguousarray(samples.T.astype(np.float32)))
    return {"waveform": waveform, "sample_rate": int(sample_rate)}


def diarize_audio(pipeline, audio_path: Path,
                   min_speakers: int, max_speakers: int) -> list[DiarizationTurn]:
    """Run a pre-loaded diarization pipeline against one audio file.

    Parameters
    ----------
    pipeline : pyannote.audio.Pipeline
        A pipeline from :func:`load_diarization_pipeline`.
    audio_path : pathlib.Path
        Path to the audio file.
    min_speakers, max_speakers : int
        Optional pyannote hints for the expected speaker count; ``0``
        means "no hint" and is omitted from the call.

    Returns
    -------
    list of DiarizationTurn
        One entry per detected speaker turn.

    Notes
    -----
    pyannote.audio 4.x's pipeline call now returns a ``DiarizeOutput``
    dataclass, not the bare ``Annotation`` 3.x returned. Uses
    ``exclusive_speaker_diarization`` (turns with overlapping speech
    resolved to a single speaker) rather than ``speaker_diarization``
    (keeps overlaps) — its own docstring calls this out as "adapted to
    downstream transcription", exactly this function's use case via
    :func:`assign_speakers`'s single-best-speaker-per-segment matching,
    where an overlapping turn would only add ambiguity.
    """
    kwargs = {}
    if min_speakers > 0:
        kwargs["min_speakers"] = min_speakers
    if max_speakers > 0:
        kwargs["max_speakers"] = max_speakers

    output = pipeline(_load_waveform_dict(audio_path), **kwargs)

    turns: list[DiarizationTurn] = []
    for turn, _, speaker in output.exclusive_speaker_diarization.itertracks(yield_label=True):
        turns.append({"start": turn.start, "end": turn.end, "speaker": speaker})
    return turns


# ── Speaker assignment (pure, no I/O) ─────────────────────────────────────────

def assign_speakers(whisper_segments: list[WhisperSegment],
                     diarization_turns: list[DiarizationTurn]) -> list[TranscriptSegment]:
    """Label each transcribed segment with its best-overlapping speaker turn.

    Parameters
    ----------
    whisper_segments : list of WhisperSegment
        Transcription segments from :func:`transcribe_audio`, ``start``/
        ``end`` in seconds.
    diarization_turns : list of DiarizationTurn
        Speaker turns from :func:`diarize_audio`, ``start``/``end`` in
        seconds plus a ``speaker`` label. May be empty (diarization was
        skipped) — every output segment then gets ``speaker=None``.

    Returns
    -------
    list of TranscriptSegment
        One entry per input segment, in order, with ``start_ms``/
        ``end_ms`` (rounded to the nearest millisecond) and ``speaker``
        set to whichever turn overlaps it most by duration, or ``None``
        if no turn overlaps it at all.

    Notes
    -----
    Max-overlap interval matching, the standard WhisperX-style recipe.
    See :doc:`/math/speaker_diarization` for the exact overlap formula.
    """
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
