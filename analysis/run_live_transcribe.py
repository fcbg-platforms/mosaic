"""
Live speech-transcription IPC server for the Real-time tab's transcript
panel. Reads raw PCM audio chunks from stdin (sent by the C++
TranscriptWorker), maintains a per-microphone rolling buffer, and
periodically writes a partial/final-transcript JSON line to stdout.

stdin protocol (per chunk) — see TranscriptWorker::submit_chunk() /
src/analysis/transcript_worker.cpp for the exact C++-side writer:
    bytes 0-3   : mic index      (int32 LE)
    bytes 4-7   : sample rate Hz (int32 LE, as actually negotiated by
                  AudioRecorder — NOT guaranteed 16kHz/mono)
    bytes 8-11  : channel count  (int32 LE)
    bytes 12-15 : sample format  (int32 LE; 0 = int16 PCM interleaved,
                  the only value emitted today — AudioRecorder always
                  normalizes to int16 before this point, see its
                  m_captureFormat doc comment)
    bytes 16-19 : sample count   (int32 LE; PER-CHANNEL frame count)
    bytes 20-23 : reserved       (int32 LE, always 0)
    bytes 24+   : sample_count * channels * 2 bytes of int16 LE PCM

Sent per-chunk rather than once at startup: keeps the protocol
self-describing per message, mirroring frame_server.py's own per-frame
width/height header even though a camera's resolution rarely changes
either — and is the only choice that's actually correct here, since this
process is started once at MainWindow construction, before AudioManager
has negotiated any mic's real format (format is only known once
AudioRecorder::start() runs).

stdout protocol (newline-terminated JSON, one line per transcript update):
    {"mic": <int>,
     "new_final_segments": [{"start_ms": <int>, "end_ms": <int>, "text": <str>}, ...],
     "tentative_text": <str>,          // current in-progress tail, replace-not-append
     "tentative_start_ms": <int>,
     "pass_ms": <float>}               // this whisper pass's wall-clock cost, diagnostic

new_final_segments is usually 0 or 1 entries; occasionally more if a long
pause lets several segments confirm in one pass. start_ms/end_ms are
cumulative audio-content-relative milliseconds since this mic's stream
began (NOT wall-clock/session time — a pause via TranscriptWorker::
set_paused() simply means no stdin bytes arrive for a while, exactly like
PoseWorker's Python side idling on a blocking stdin read while paused; see
pose_worker.hpp's set_paused() doc comment). The C++ side stamps its own
wall-clock receive time for display instead of interpreting these directly
— see TranscriptPanelW::push_final().

    python analysis/run_live_transcribe.py --model tiny

See analysis/README.rst for full documentation.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from pathlib import Path

# Same HF_HOME redirect convention as run_diarize.py, done before the first
# huggingface_hub-backed import (transcribe/windowing.py has none, but
# load_whisper_model() below does) — see that file's own comment for why
# this must happen at module load, ahead of everything else.
_MODELS_DIR = Path(__file__).parent / "diarize" / "models"
_MODELS_DIR.mkdir(parents=True, exist_ok=True)
os.environ.setdefault("HF_HOME", str(_MODELS_DIR))

from diarize.pipeline import load_whisper_model, resolve_device  # noqa: E402
from transcribe.resample import pcm16_to_mono_float32, resample_to_16k  # noqa: E402
from transcribe.windowing import Segment, confirm_segments, trim_buffer_samples  # noqa: E402

import numpy as np  # noqa: E402

_HEADER_FMT = "<iiiiii"   # mic, sample_rate, channels, sample_format, sample_count, reserved
_HEADER_SIZE = struct.calcsize(_HEADER_FMT)

WHISPER_SAMPLE_RATE = 16000
STEP_SEC = 1.5              # minimum new audio required before a pass is triggered
WINDOW_MAX_SEC = 10.0        # hard cap on rolling-buffer length (safety valve)
TRAILING_MARGIN_SEC = 1.0    # confirm rule margin (see windowing.py docstring)
MIN_AUDIO_SEC = 0.5          # skip a pass if the buffer is shorter than this


class MicState:
    def __init__(self) -> None:
        self.buffer = np.zeros(0, dtype=np.float32)   # mono, 16kHz
        self.buf_start_ms = 0.0                         # absolute ms of buffer[0]
        self.new_audio_sec = 0.0                         # since last pass, gates STEP_SEC


def read_chunk() -> tuple[int, int, int, np.ndarray] | tuple[None, None, None, None]:
    """Reads one header + PCM payload from stdin, returning the mic index,
    sample rate, channel count, and the payload already downmixed to mono
    and resampled to 16kHz. (None, None, None, None) on EOF/short read."""
    header = sys.stdin.buffer.read(_HEADER_SIZE)
    if len(header) < _HEADER_SIZE:
        return None, None, None, None

    mic, sample_rate, channels, sample_format, sample_count, _reserved = struct.unpack(
        _HEADER_FMT, header)
    n_bytes = sample_count * channels * 2
    payload = sys.stdin.buffer.read(n_bytes)
    if len(payload) < n_bytes:
        return None, None, None, None

    if sample_format != 0:
        # Only int16 PCM is emitted today; fail loudly rather than silently
        # misinterpret bytes if TranscriptWorker's writer ever changes.
        raise ValueError(f"unsupported sample_format {sample_format}")

    mono = pcm16_to_mono_float32(payload, channels)
    mono16k = resample_to_16k(mono, sample_rate)
    return mic, sample_rate, channels, mono16k


def run_pass(model, mic: int, state: MicState) -> dict | None:
    """Runs one whisper pass over state's current buffer, confirms/trims it,
    and returns the JSON-ready result dict, or None if there isn't enough
    audio yet to bother running."""
    buffer_duration_sec = len(state.buffer) / WHISPER_SAMPLE_RATE
    if buffer_duration_sec < MIN_AUDIO_SEC:
        return None

    t0 = time.perf_counter()
    segments_iter, _info = model.transcribe(
        state.buffer, language=None, vad_filter=True,
        condition_on_previous_text=False, beam_size=1,
    )
    segments = [Segment(s.start, s.end, s.text.strip()) for s in segments_iter]
    pass_ms = (time.perf_counter() - t0) * 1000.0

    confirmed, tentative_text, watermark = confirm_segments(
        segments, buffer_duration_sec, TRAILING_MARGIN_SEC)

    # Safety valve: if nothing confirmed and the buffer is at its cap, force
    # a trim to bound growth (documented limitation — see windowing.py).
    if not confirmed and buffer_duration_sec >= WINDOW_MAX_SEC:
        watermark = max(0.0, buffer_duration_sec - TRAILING_MARGIN_SEC)

    new_final_segments = [
        {"start_ms": round(state.buf_start_ms + s.start_sec * 1000),
         "end_ms":   round(state.buf_start_ms + s.end_sec * 1000),
         "text":     s.text}
        for s in confirmed
    ]

    if watermark > 0.0:
        drop_n = trim_buffer_samples(watermark, WHISPER_SAMPLE_RATE)
        state.buffer = state.buffer[drop_n:]
        state.buf_start_ms += watermark * 1000

    return {
        "mic": mic,
        "new_final_segments": new_final_segments,
        "tentative_text": tentative_text,
        "tentative_start_ms": round(state.buf_start_ms),
        "pass_ms": round(pass_ms, 1),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="MOSAIC live transcription IPC server")
    parser.add_argument("--model", default="tiny",
                         choices=["tiny", "base", "small", "medium", "large-v3"])
    parser.add_argument("--device", default=None)
    args = parser.parse_args()

    device = resolve_device(args.device)
    print(f"[run_live_transcribe] Loading whisper model={args.model} device={device}...",
          file=sys.stderr, flush=True)
    model = load_whisper_model(args.model, device)
    print("[run_live_transcribe] Ready.", file=sys.stderr, flush=True)

    mics: dict[int, MicState] = {}
    while True:
        mic, sample_rate, channels, mono16k = read_chunk()
        if mic is None:
            break

        state = mics.setdefault(mic, MicState())
        state.buffer = np.concatenate([state.buffer, mono16k])
        state.new_audio_sec += len(mono16k) / WHISPER_SAMPLE_RATE

        if state.new_audio_sec >= STEP_SEC:
            state.new_audio_sec = 0.0
            result = run_pass(model, mic, state)
            if result is not None:
                sys.stdout.write(json.dumps(result) + "\n")
                sys.stdout.flush()


if __name__ == "__main__":
    main()
