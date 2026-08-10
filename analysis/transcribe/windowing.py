"""
Pure windowing/confirmation logic for live transcription's rolling buffer.
No torch/faster-whisper import here — mirrors analysis/diarize/pipeline.py's
assign_speakers() precedent of isolating the one pure/testable piece from
the real model call.

Algorithm ("trailing-margin confirmation"):
Whisper is re-run over the *entire* current rolling audio buffer on every
pass (not incrementally). Each pass returns a list of (start, end, text)
segments in buffer-relative seconds, VAD-segmented by faster-whisper's own
vad_filter=True (segment boundaries land on natural speech pauses, which is
what keeps words from being cut mid-word at a segment boundary).

A segment is "confirmed" (final, never revised again) once its end lies at
least TRAILING_MARGIN_SEC before the buffer's current end — i.e. it had a
margin of trailing audio context on both the previous pass and this one, so
its wording is stable. Everything after that point is "tentative" text,
replaced wholesale on every pass. Confirmed audio is then trimmed off the
front of the buffer so growth is bounded.

This intentionally skips the more elaborate cross-pass textual-agreement
("LocalAgreement-n") policy some streaming-ASR projects use: VAD-anchored
segment boundaries are already stable in practice for the confirmed prefix,
and the simpler rule is enough to implement/verify directly for a tiny-model
v1. Known limitation: a single unbroken utterance longer than the caller's
own hard buffer cap (see run_live_transcribe.py's WINDOW_MAX_SEC) with no
detected VAD gap will never confirm mid-utterance; the caller forces a trim
at that point anyway to bound memory, at the cost of a possibly-truncated
final segment in that rare case — documented, not silently handled.
"""
from __future__ import annotations

from typing import NamedTuple


class Segment(NamedTuple):
    start_sec: float
    end_sec: float
    text: str


def confirm_segments(
    segments: list[Segment],
    buffer_duration_sec: float,
    trailing_margin_sec: float,
) -> tuple[list[Segment], str, float]:
    """Splits one whisper pass's segments into confirmed vs. tentative.

    Parameters
    ----------
    segments : list of Segment
        This pass's segments, in buffer-relative seconds, chronological
        (faster-whisper always returns them in order).
    buffer_duration_sec : float
        Current rolling buffer's total length in seconds.
    trailing_margin_sec : float
        A segment is confirmable only if it ends at least this long before
        buffer_duration_sec.

    Returns
    -------
    (confirmed, tentative_text, watermark_sec)
        confirmed : the newly-final segments this pass (still
            buffer-relative — caller adds its own running offset to get
            absolute time).
        tentative_text : confirmed segments' texts are NOT included;
            everything after the last confirmed segment, space-joined.
        watermark_sec : confirmed[-1].end_sec, or 0.0 if nothing confirmed
            this pass — caller trims the buffer's front up to this point.
    """
    cutoff = buffer_duration_sec - trailing_margin_sec
    confirmed = [s for s in segments if s.end_sec <= cutoff]
    tentative = [s for s in segments if s.end_sec > cutoff]
    tentative_text = " ".join(s.text for s in tentative).strip()
    watermark = confirmed[-1].end_sec if confirmed else 0.0
    return confirmed, tentative_text, watermark


def trim_buffer_samples(confirmed_watermark_sec: float, sample_rate_hz: int) -> int:
    """Sample count to drop from the front of the rolling buffer, given how
    far (in seconds) audio has been confirmed. Pure arithmetic, split out
    only so the "seconds -> sample index" conversion has one, tested home
    rather than being re-derived at each call site."""
    return int(confirmed_watermark_sec * sample_rate_hz)
