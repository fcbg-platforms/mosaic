"""
MOSAIC speaker diarization + transcription runner — transcribes recorded
session audio with faster-whisper and, when a Hugging Face token is
available, labels each segment with a speaker via pyannote.audio. Never
touches the original .wav files: writes a sidecar "<name>.transcript.json"
next to each one.

    python analysis/run_diarize.py --session /path/to/session_2026-06-07_14-30
    python analysis/run_diarize.py --audio /path/to/audio/audio_0.wav

See analysis/README.rst for full documentation.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

# Auto-downloaded model weights (faster-whisper + pyannote) are redirected
# into a local, gitignored cache dir — same convention as
# analysis/facemask/models/ — instead of the global ~/.cache/huggingface.
# Must be set before the first huggingface_hub-backed import happens (i.e.
# before diarize.pipeline's lazy `from faster_whisper import ...` /
# `from pyannote.audio import ...` calls), so it's done here at module load,
# ahead of everything else in main().
_MODELS_DIR = Path(__file__).parent / "diarize" / "models"
_MODELS_DIR.mkdir(parents=True, exist_ok=True)
os.environ.setdefault("HF_HOME", str(_MODELS_DIR))

from diarize.pipeline import (  # noqa: E402
    assign_speakers,
    diarize_audio,
    load_diarization_pipeline,
    load_whisper_model,
    resolve_device,
    resolve_diarization_status,
    transcribe_audio,
)

# ── CLI ───────────────────────────────────────────────────────────────────────


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MOSAIC transcription + speaker diarization")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--session",
        metavar="DIR",
        help="Transcribe/diarize all .wav files in a recorded session directory",
    )
    group.add_argument("--audio", metavar="FILE", help="Transcribe/diarize a single audio file")

    parser.add_argument(
        "--model",
        choices=["tiny", "base", "small", "medium", "large-v3"],
        default="small",
        help="faster-whisper model size (default: small)",
    )
    parser.add_argument(
        "--language", default=None, help="Force a language code (e.g. 'en'); default auto-detects"
    )
    parser.add_argument(
        "--device", default=None, help="cpu / cuda — default auto-selects cuda if available"
    )
    parser.add_argument(
        "--hf-token",
        default=None,
        help="Hugging Face access token for pyannote diarization models. "
        "Falls back to the HF_TOKEN / HUGGINGFACE_TOKEN env vars.",
    )
    parser.add_argument(
        "--min-speakers", type=int, default=0, help="Optional hint for pyannote (0 = unset)"
    )
    parser.add_argument(
        "--max-speakers", type=int, default=0, help="Optional hint for pyannote (0 = unset)"
    )
    parser.add_argument(
        "--skip-diarization",
        action="store_true",
        help="Transcript only — skip diarization even if a token is available",
    )
    return parser.parse_args()


# ── Model loading (once per run, shared across every audio file) ───────────────


def _load_models(model_size: str, device: str | None, hf_token: str | None, skip_diarization: bool):
    """Resolves the device and loads the whisper model plus (optionally) the
    diarization pipeline exactly once. process_audio() used to build both
    fresh on every call, which reloaded weights from disk/cache once per
    microphone in a multi-mic session instead of once per run. Returns
    (resolved_device, whisper_model, diarization_pipeline_or_None)."""
    resolved_device = resolve_device(device)
    token = hf_token or os.environ.get("HF_TOKEN") or os.environ.get("HUGGINGFACE_TOKEN")

    print(
        f"[run_diarize] Loading whisper model (model={model_size}, "
        f"device={resolved_device})...",
        flush=True,
    )
    whisper_model = load_whisper_model(model_size, resolved_device)

    diarization_pipeline = None
    load_status = "ok"
    load_detail = ""
    if skip_diarization:
        load_status = "skipped_by_user"
        load_detail = "Transcript-only was requested."
        print(
            "[run_diarize] Skipping diarization: --skip-diarization requested "
            "— transcript only.",
            flush=True,
        )
    elif not token:
        load_status = "no_token"
        load_detail = "No Hugging Face token was provided."
        print(
            "[run_diarize] Skipping diarization: no Hugging Face token provided "
            "— transcript only. Pass --hf-token or set HF_TOKEN to enable "
            "speaker labels.",
            flush=True,
        )
    else:
        print("[run_diarize] Loading diarization pipeline...", flush=True)
        try:
            diarization_pipeline = load_diarization_pipeline(token, resolved_device)
        except Exception as exc:  # noqa: BLE001 - degrade to transcript-only, not a hard failure
            load_status = "load_failed"
            # Kept verbatim rather than summarised: the case worth diagnosing
            # is a 401 from a token that exists but whose owner never accepted
            # the gated model's terms, and only the raw message says so.
            load_detail = str(exc)
            print(f"[run_diarize] Diarization pipeline unavailable: {exc}", file=sys.stderr)

    return resolved_device, whisper_model, diarization_pipeline, (load_status, load_detail)


# ── Session mode ─────────────────────────────────────────────────────────────


def process_session(
    session_dir: Path,
    model_size: str,
    language: str | None,
    device: str | None,
    hf_token: str | None,
    min_speakers: int,
    max_speakers: int,
    skip_diarization: bool,
) -> None:
    audios = sorted((session_dir / "audio").glob("*.wav"))
    if not audios:
        print(f"[run_diarize] No .wav files found in {session_dir / 'audio'}", file=sys.stderr)
        sys.exit(1)

    print(f"[run_diarize] Found {len(audios)} audio file(s) in {session_dir / 'audio'}", flush=True)

    _device, whisper_model, diarization_pipeline, load_outcome = _load_models(
        model_size, device, hf_token, skip_diarization
    )

    failures = 0
    for audio_path in audios:
        out_path = audio_path.with_suffix("").with_suffix(".transcript.json")
        if not process_audio(
            audio_path,
            out_path,
            model_size,
            whisper_model,
            diarization_pipeline,
            language,
            min_speakers,
            max_speakers,
            load_outcome,
        ):
            failures += 1

    if failures:
        print(
            f"[run_diarize] {failures}/{len(audios)} file(s) failed to process "
            f"— see errors above.",
            file=sys.stderr,
        )
        sys.exit(1)


def process_audio(
    audio_path: Path,
    out_path: Path,
    model_size: str,
    whisper_model,
    diarization_pipeline,
    language: str | None,
    min_speakers: int,
    max_speakers: int,
    load_outcome: tuple[str, str],
) -> bool:
    """Returns True on success. whisper_model/diarization_pipeline come from
    _load_models() (diarization_pipeline may be None, meaning skip it —
    already-logged reason). Always writes a transcript-only result on a
    diarization failure (gated-model auth error, network issue) rather than
    failing the whole run — the transcript itself is still useful. Only a
    transcription failure (unreadable/corrupt audio) is a hard failure."""
    print(f"[run_diarize] Transcribing {audio_path.name} (model={model_size})...", flush=True)
    t_start = time.perf_counter()
    try:
        whisper_segments, detected_language = transcribe_audio(whisper_model, audio_path, language)
    except Exception as exc:  # noqa: BLE001 - unreadable/corrupt audio is a hard failure
        print(f"[run_diarize] Failed to transcribe {audio_path.name}: {exc}", file=sys.stderr)
        return False

    for seg in whisper_segments:
        preview = seg["text"][:60] + ("…" if len(seg["text"]) > 60 else "")
        print(f"  [{seg['start']:.1f}s-{seg['end']:.1f}s] {preview}", flush=True)

    diarization_ran = False
    diarization_turns: list = []
    run_error: str | None = None
    if diarization_pipeline is None:
        print(
            f"[run_diarize] No diarization pipeline available for {audio_path.name} "
            f"— transcript only.",
            flush=True,
        )
    else:
        print(f"[run_diarize] Diarizing {audio_path.name}...", flush=True)
        try:
            diarization_turns = diarize_audio(
                diarization_pipeline, audio_path, min_speakers, max_speakers
            )
            diarization_ran = True
        except Exception as exc:  # noqa: BLE001 - degrade to transcript-only, not a hard failure
            run_error = str(exc)
            print(f"[run_diarize] Diarization failed for {audio_path.name}: {exc}", file=sys.stderr)

    segments = assign_speakers(whisper_segments, diarization_turns)

    # Persist *why*, not just whether. Without this the reason lives only in
    # this run's stdout while the consequence — a transcript with no speakers —
    # lives on disk forever, and the app has no way to explain itself later.
    status, detail = resolve_diarization_status(
        load_status=load_outcome[0],
        load_detail=load_outcome[1],
        run_error=run_error,
        turn_count=len(diarization_turns),
        labeled_segment_count=sum(1 for seg in segments if seg["speaker"]),
    )
    if status != "ok":
        print(f"[run_diarize] No speaker labels ({status}): {detail}", flush=True)

    out_path.write_text(
        json.dumps(
            {
                "source_audio": audio_path.name,
                "model": model_size,
                "language": detected_language,
                "diarization": diarization_ran,
                "diarization_status": status,
                "diarization_detail": detail,
                "segments": segments,
            },
            indent=2,
        ),
        encoding="utf-8",
    )

    elapsed = time.perf_counter() - t_start
    print(
        f"[run_diarize] Done. {len(segments)} segment(s) in {elapsed:.1f}s → {out_path}", flush=True
    )
    return True


# ── Entry point ───────────────────────────────────────────────────────────────


def main() -> None:
    args = parse_args()

    if args.session:
        process_session(
            Path(args.session),
            args.model,
            args.language,
            args.device,
            args.hf_token,
            args.min_speakers,
            args.max_speakers,
            args.skip_diarization,
        )
    elif args.audio:
        audio_path = Path(args.audio)
        out_path = audio_path.with_suffix("").with_suffix(".transcript.json")
        _device, whisper_model, diarization_pipeline, load_outcome = _load_models(
            args.model, args.device, args.hf_token, args.skip_diarization
        )
        if not process_audio(
            audio_path,
            out_path,
            args.model,
            whisper_model,
            diarization_pipeline,
            args.language,
            args.min_speakers,
            args.max_speakers,
            load_outcome,
        ):
            sys.exit(1)


if __name__ == "__main__":
    main()
