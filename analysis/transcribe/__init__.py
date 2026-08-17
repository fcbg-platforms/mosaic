"""Pure windowing/confirmation logic and PCM resampling for the Real-time
tab's live transcription worker (analysis/run_live_transcribe.py)."""

from .resample import WHISPER_SAMPLE_RATE, pcm16_to_mono_float32, resample_to_16k
from .windowing import Segment, confirm_segments, trim_buffer_samples

__all__ = [
    "Segment",
    "confirm_segments",
    "trim_buffer_samples",
    "pcm16_to_mono_float32",
    "resample_to_16k",
    "WHISPER_SAMPLE_RATE",
]
