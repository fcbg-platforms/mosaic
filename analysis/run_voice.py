"""
Acoustic analysis for a recorded session: spectrogram, pitch track and
intensity contour, one pair of sidecars per microphone.

Writes "<name>.voice.png" (8-bit greyscale, dB-normalised) and
"<name>.voice.json" (axes, dB range, and the two tracks) beside each source
WAV. Original recordings are never modified.

Deliberately separate from run_diarize.py. This pass needs no models, no
downloads and no Hugging Face token, and finishes in seconds where a Whisper
transcription takes minutes — so an operator can get an acoustic view of a
session without re-transcribing it, and can get one at all for a session that
was never diarized.

Usage:
    python run_voice.py --session /path/to/session
    python run_voice.py --audio /path/to/audio.wav
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))

from voice.spectro import (  # noqa: E402
    SPECTROGRAM_ROWS,
    accumulate_column_max,
    auto_pitch_range,
    column_indices,
    db_to_uint8,
    decimate_track,
    drop_short_voiced_runs,
    dynamic_range,
    fill_empty_columns,
    reduce_freq_mean,
    spectrogram_time_step,
    suppress_low_intensity_pitch,
    target_columns,
    track_step_ms,
)

#: Bumped only when the meaning of an existing field changes. VoiceResult::load
#: in src/analysis/voice_result.cpp refuses anything else rather than
#: misinterpreting an image's pixels.
SCHEMA_VERSION = 1

#: Row 0 of the PNG is the highest frequency. Recorded in the JSON and checked
#: by the C++ because an upside-down spectrogram is entirely plausible-looking.
ROW_ORDER = "high_to_low"

DEFAULT_MAX_FREQ_HZ = 8000.0
TRACK_STEP_S = 0.01


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Spectrogram, pitch and intensity for a session.")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--session", help="Session directory (processes every WAV in audio/)")
    source.add_argument("--audio", help="A single .wav file")
    parser.add_argument(
        "--max-freq", type=float, default=DEFAULT_MAX_FREQ_HZ, help="Spectrogram ceiling in Hz"
    )
    parser.add_argument("--pitch-floor", type=float, default=60.0)
    parser.add_argument("--pitch-ceiling", type=float, default=600.0)
    parser.add_argument(
        "--auto-pitch-range",
        action="store_true",
        help="Narrow the pitch range to the voices actually present (two-pass)",
    )
    parser.add_argument(
        "--force", action="store_true", help="Recompute even if sidecars already exist"
    )
    return parser.parse_args()


def process_audio(audio_path: Path, args: argparse.Namespace) -> bool:
    """Returns True on success. Writes both sidecars, or neither."""
    from PIL import Image
    from voice.praat import (
        CHUNK_SECONDS,
        compute_intensity,
        compute_pitch,
        compute_spectrogram,
        effective_max_frequency,
        iter_chunks,
        load_sound,
    )

    json_path = audio_path.with_suffix("").with_suffix(".voice.json")
    png_path = audio_path.with_suffix("").with_suffix(".voice.png")
    # Both sidecars, not just the JSON. Checking only the JSON meant a deleted
    # or truncated PNG could never be regenerated from the UI — which passes no
    # --force — so the panel stayed permanently blank while the run cheerfully
    # reported "Done."
    if json_path.exists() and png_path.exists() and not args.force:
        print(f"[run_voice] {json_path.name} already exists — skipping (use --force)", flush=True)
        return True

    t_start = time.perf_counter()
    snd = load_sound(audio_path)
    duration_s = float(snd.duration)
    max_freq = effective_max_frequency(snd, args.max_freq)
    time_step = spectrogram_time_step(duration_s)
    n_cols = target_columns(duration_s)

    print(
        f"[run_voice] {audio_path.name}: {duration_s:.1f}s, "
        f"{n_cols} columns, 0-{max_freq:.0f} Hz, step {time_step * 1000:.0f} ms",
        flush=True,
    )

    # ── Spectrogram, chunk by chunk ──────────────────────────────────────────
    # Reduced into its final grid as each chunk arrives, never accumulated at
    # full resolution. Concatenating the chunks first would rebuild exactly the
    # whole-file matrix the chunking exists to avoid: 2.8 GB peak for a
    # 30-minute recording, against 13 MB for this grid.
    out_db: np.ndarray | None = None
    filled = np.zeros(n_cols, dtype=bool)
    freqs: np.ndarray | None = None
    n_chunks = int(np.ceil(duration_s / CHUNK_SECONDS)) or 1

    # The generator is consumed lazily; list() would materialise every padded
    # chunk Sound at once for no benefit.
    for i, (chunk, keep_from, keep_to) in enumerate(iter_chunks(snd)):
        spec = compute_spectrogram(chunk, max_frequency=max_freq, time_step=time_step)
        # Drop the padding columns; the analysis window could not span the chunk
        # boundary, so keeping them would put a seam every 30 seconds.
        keep = (spec.times_s >= keep_from) & (spec.times_s < keep_to)
        if keep.any():
            db = spec.db[:, keep]
            if out_db is None:
                out_db = np.zeros((db.shape[0], n_cols), dtype=np.float64)
                freqs = spec.freqs_hz
            cols = column_indices(spec.times_s[keep], n_cols, 0.0, duration_s)
            accumulate_column_max(out_db, filled, db, cols)
        print(f"  {(i + 1) / n_chunks * 100:.1f}%  ({i + 1}/{n_chunks})", flush=True)

    image_written = False
    db_min = db_max = 0.0
    img_w = img_h = 0
    # The ceiling actually analysed, which is not necessarily the one asked for:
    # parselmouth picks its own bin layout, and declaring the requested value
    # would mislabel the frequency axis by however much it differs.
    actual_max_hz = max_freq
    if out_db is not None and freqs is not None:
        fill_empty_columns(out_db, filled)
        # Back to power for the frequency average, which must not happen in dB.
        rows = reduce_freq_mean(np.power(10.0, out_db / 10.0), SPECTROGRAM_ROWS)
        db_rows = 10.0 * np.log10(np.maximum(rows, 1e-14))

        db_min, db_max = dynamic_range(db_rows)
        img = db_to_uint8(db_rows, db_min, db_max)
        # Row 0 must be the highest frequency; parselmouth returns ascending Hz.
        Image.fromarray(np.flipud(img), mode="L").save(png_path)
        image_written = True
        # The *written* dimensions, never the intended ones. reduce_freq_mean
        # passes the input through when it already has fewer rows than the
        # target, so a low ceiling or a coarse frequency step yields a shorter
        # image than SPECTROGRAM_ROWS — and metadata that disagrees with the
        # file it describes is how a spectrogram ends up drawn against the wrong
        # frequency axis.
        img_h, img_w = int(img.shape[0]), int(img.shape[1])
        actual_max_hz = float(freqs[-1])
        print(f"[run_voice] Spectrogram → {png_path.name} ({img_w}x{img_h})", flush=True)

    # ── Pitch and intensity, whole-file ──────────────────────────────────────
    # Not chunked: both are one value per frame rather than a matrix, so the
    # memory problem that forces chunking above does not arise.
    int_times, int_db = compute_intensity(snd, time_step=TRACK_STEP_S)

    floor_hz, ceiling_hz = args.pitch_floor, args.pitch_ceiling
    range_auto = False
    pitch_times, pitch_hz = compute_pitch(
        snd, time_step=TRACK_STEP_S, floor_hz=floor_hz, ceiling_hz=ceiling_hz
    )
    if args.auto_pitch_range:
        narrowed = auto_pitch_range(pitch_hz, fallback=(floor_hz, ceiling_hz))
        if narrowed != (floor_hz, ceiling_hz):
            floor_hz, ceiling_hz = narrowed
            range_auto = True
            pitch_times, pitch_hz = compute_pitch(
                snd, time_step=TRACK_STEP_S, floor_hz=floor_hz, ceiling_hz=ceiling_hz
            )

    # Resampled onto the pitch frame times rather than compared index-for-index:
    # Praat gives the two analyses different window lengths (3/pitch_floor vs
    # 32 ms), so frame i of one is not frame i of the other, and the offset
    # changes again when --auto-pitch-range re-runs with a narrowed floor.
    int_at_pitch = (
        np.interp(pitch_times, int_times, int_db)
        if int_times.size and pitch_times.size
        else np.zeros_like(pitch_hz)
    )
    pitch_hz = suppress_low_intensity_pitch(pitch_hz, int_at_pitch)
    pitch_hz = drop_short_voiced_runs(pitch_hz)

    step_ms = track_step_ms(duration_s)
    factor = max(1, int(round(step_ms / (TRACK_STEP_S * 1000.0))))
    pitch_out = decimate_track(pitch_hz, factor, voiced_aware=True)
    int_out = decimate_track(int_db, factor)

    # A decimated sample represents its whole block, so it belongs at the
    # block's centre. Timestamping it at the leading edge shifts both contours
    # early by (factor-1)/2 frames — 25 ms on an hour-long recording — against
    # a spectrogram whose first-order requirement is that everything lines up.
    block_offset_ms = (factor - 1) * TRACK_STEP_S * 1000.0 / 2.0

    payload = {
        "schema_version": SCHEMA_VERSION,
        "source_audio": audio_path.name,
        "duration_ms": round(duration_s * 1000.0, 1),
        "sample_rate": int(snd.sampling_frequency),
        "spectrogram": {
            "image": png_path.name if image_written else "",
            "width": img_w,
            "height": img_h,
            "row_order": ROW_ORDER,
            "t0_ms": 0.0,
            "t1_ms": round(duration_s * 1000.0, 1),
            "f0_hz": 0.0,
            "f1_hz": round(actual_max_hz, 1),
            "db_min": round(float(db_min), 2),
            "db_max": round(float(db_max), 2),
        },
        "pitch": {
            "t0_ms": round(float(pitch_times[0]) * 1000.0 + block_offset_ms, 1)
            if pitch_times.size
            else 0.0,
            "dt_ms": step_ms,
            "floor_hz": round(floor_hz, 1),
            "ceiling_hz": round(ceiling_hz, 1),
            "range_auto": range_auto,
            "unvoiced_value": 0.0,
            "values_hz": [round(float(v), 2) for v in pitch_out],
        },
        "intensity": {
            "t0_ms": round(float(int_times[0]) * 1000.0 + block_offset_ms, 1)
            if int_times.size
            else 0.0,
            "dt_ms": step_ms,
            "values_db": [round(float(v), 2) for v in int_out],
        },
    }
    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    voiced = int(np.sum(pitch_out > 0))
    print(
        f"[run_voice] Acoustics → {json_path.name} "
        f"({len(pitch_out)} frames, {voiced} voiced, {time.perf_counter() - t_start:.1f}s)",
        flush=True,
    )
    return True


def main() -> None:
    args = parse_args()

    # Checked here rather than at import so the message is actionable: a bare
    # ImportError traceback in the app's log view says nothing about how to fix
    # it, and the venv genuinely will not have this package until a sync runs.
    try:
        import parselmouth  # noqa: F401
    except ImportError as exc:
        print(
            f"[run_voice] ERROR: praat-parselmouth is not installed in analysis/.venv ({exc}).\n"
            f"[run_voice] Run:  uv sync --project analysis",
            file=sys.stderr,
            flush=True,
        )
        sys.exit(2)

    if args.session:
        audio_dir = Path(args.session) / "audio"
        audios = sorted(audio_dir.glob("*.wav"))
        if not audios:
            print(f"[run_voice] No .wav files found in {audio_dir}", file=sys.stderr)
            sys.exit(1)
        print(f"[run_voice] Found {len(audios)} audio file(s) in {audio_dir}", flush=True)

        failures = 0
        for i, audio_path in enumerate(audios):
            print(f"[run_voice] Mic {i + 1}/{len(audios)}: {audio_path.name}", flush=True)
            try:
                if not process_audio(audio_path, args):
                    failures += 1
            except Exception as exc:  # noqa: BLE001 - one bad mic must not lose the rest
                failures += 1
                print(f"[run_voice] Failed on {audio_path.name}: {exc}", file=sys.stderr)
        if failures:
            print(f"[run_voice] {failures}/{len(audios)} file(s) failed.", file=sys.stderr)
            sys.exit(1)
    else:
        if not process_audio(Path(args.audio), args):
            sys.exit(1)


if __name__ == "__main__":
    main()
