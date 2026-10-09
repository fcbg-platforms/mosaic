"""
MOSAIC remote heart-rate (rPPG) analysis runner — extracts a per-window
heart-rate estimate from face video using classical (non-deep-learning)
signal-processing algorithms (Green/CHROM/POS). In --session mode, writes
one "<name>.<backend>.rppg.json" per camera into the session's own rppg/
subfolder; in standalone --video mode, writes it beside the source video.
Originals are never modified.

    python analysis/run_rppg.py --session /path/to/session_2026-06-07_14-30
    python analysis/run_rppg.py --video /path/to/video/video_0.mp4 --backend pos

EXPERIMENTAL — this is a research-grade heart-rate estimate only, not a
medical device and not clinically validated. See analysis/README.rst for
full documentation and the accuracy caveats surfaced in the app's own UI.

No --skip option, unlike sibling analysis scripts: skipping frames would
downsample the pulse signal itself, and Nyquist for a 3 Hz upper
physiological bound already needs >6 Hz effective sampling — a typical
15-25 fps camera has only marginal headroom even processing every frame.
Frame-skip genuinely doesn't fit this domain's cost structure the way it
does for pose/expression (which only thin out *recorded* keypoints, not a
signal being frequency-analyzed).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

import cv2
import numpy as np
from rppg import hrv
from rppg.algorithms import BACKENDS
from rppg.hr_estimation import bandpass_filter, estimate_hr_welch, median_smooth
from rppg.roi import MediaPipeFaceRoiExtractor

LOW_HZ = 0.7  # 42 BPM
HIGH_HZ = 3.0  # 180 BPM
MIN_VALID_FRACTION = 0.6  # a window needs >=60% of its frames face-detected to attempt HR

# ── CLI ───────────────────────────────────────────────────────────────────────


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="MOSAIC remote heart-rate (rPPG) analysis "
        "— EXPERIMENTAL, not clinically validated"
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--session", metavar="DIR", help="Process all .mp4 files in a recorded session directory"
    )
    group.add_argument("--video", metavar="FILE", help="Process a single video file")

    parser.add_argument(
        "--backend",
        choices=["green", "chrom", "pos"],
        default="pos",
        help="Signal-combination algorithm: naive green-channel baseline "
        "(fastest, most motion-sensitive), CHROM (chrominance-based, "
        "de Haan & Jeanne 2013), or POS (Plane-Orthogonal-to-Skin, "
        "Wang et al. 2017 — default, generally the most robust classical "
        "method)",
    )
    parser.add_argument(
        "--window-sec",
        type=float,
        default=10.0,
        help="HR-analysis window length in seconds (default: 10.0)",
    )
    parser.add_argument(
        "--hop-sec",
        type=float,
        default=2.0,
        help="Sliding-window hop length in seconds (default: 2.0)",
    )
    parser.add_argument(
        "--smoothing-windows",
        type=int,
        default=1,
        help="Centered median-filter width, in windows, for the smoothed_bpm "
        "series (default: 1 = no smoothing; raw bpm is always also kept)",
    )
    parser.add_argument(
        "--min-confidence",
        type=float,
        default=0.5,
        help="Face detection/presence confidence threshold (default: 0.5)",
    )
    return parser.parse_args()


# ── Session mode ─────────────────────────────────────────────────────────────


def _camera_index_from_filename(video_path: Path) -> int:
    """Parses the real camera index from a MOSAIC-recorded video's own
    filename (e.g. "video_2.mp4" -> 2) — mirrors run_pose.py's/
    run_expression.py's identical helper/rationale (VideoManager preserves
    gaps in camera indices when an earlier camera fails to open during
    recording, so a plain enumerate() over a sorted file list can silently
    assign the wrong index). Falls back to 0 with a stderr warning if the
    filename doesn't match "video_N.<ext>"."""
    m = re.search(r"video_(\d+)", video_path.stem)
    if not m:
        print(
            f"[run_rppg] Warning: could not parse a camera index from "
            f"{video_path.name}, defaulting to 0",
            file=sys.stderr,
        )
        return 0
    return int(m.group(1))


def process_session(
    session_dir: Path,
    backend: str,
    window_sec: float,
    hop_sec: float,
    smoothing_windows: int,
    min_confidence: float,
) -> None:
    videos = sorted((session_dir / "video").glob("*.mp4"))
    if not videos:
        print(f"[run_rppg] No .mp4 files found in {session_dir / 'video'}", file=sys.stderr)
        sys.exit(1)

    print(f"[run_rppg] Found {len(videos)} video(s) in {session_dir / 'video'}", flush=True)

    # Own subfolder, mirrors run_pose.py's pose_dir / run_expression.py's
    # expression_dir convention. Forward-only: sessions analyzed before
    # this feature existed have no rppg/ folder and won't retroactively
    # gain one (this project's established no-migration convention).
    rppg_dir = session_dir / "rppg"

    # Built once, reused across every video in the session — the same
    # redundant-model-reload mistake item 17's diarization plugin had to
    # fix after code review, avoided here from the start.
    extractor = MediaPipeFaceRoiExtractor(min_confidence=min_confidence)

    for position, video_path in enumerate(videos, start=1):
        camera_index = _camera_index_from_filename(video_path)
        print(f"[run_rppg] Camera {position}/{len(videos)}: {video_path.name}", flush=True)
        process_video(
            video_path,
            extractor,
            backend,
            window_sec,
            hop_sec,
            smoothing_windows,
            camera_index=camera_index,
            output_dir=rppg_dir,
        )


def _read_timestamps_ms(video_path: Path, camera_index: int) -> list[float]:
    # Built directly from camera_index (not a naive "video" -> "timestamps_cam"
    # substring-replace on the video's own stem) — that substitution turns
    # "video_0" into "timestamps_cam_0.csv" (stray underscore), which never
    # matches the real "timestamps_cam0.csv" file. Same bug already found
    # and fixed in run_pose.py/run_pose3d.py/run_gaze_fusion.py/
    # run_expression.py this session.
    ts_csv = video_path.with_name(f"timestamps_cam{camera_index}.csv")
    timestamps_ms: list[float] = []
    if ts_csv.exists():
        import csv

        with ts_csv.open() as f:
            for row in csv.DictReader(f):
                try:
                    timestamps_ms.append(int(row.get("elapsed_ns", 0)) / 1e6)
                except ValueError:
                    timestamps_ms.append(0.0)
    return timestamps_ms


def process_video(
    video_path: Path,
    extractor: MediaPipeFaceRoiExtractor,
    backend: str,
    window_sec: float,
    hop_sec: float,
    smoothing_windows: int,
    camera_index: int = 0,
    output_dir: Path | None = None,
) -> None:
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        print(f"[run_rppg] Cannot open {video_path}", file=sys.stderr)
        return

    fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    print(
        f"[run_rppg] Processing {video_path.name}  ({total} frames @ {fps:.1f} fps, "
        f"backend={backend})",
        flush=True,
    )

    # Scales to the video's own length instead of a fixed stride, matching
    # every sibling analysis script's identical fix.
    progress_interval = max(1, total // 200)

    timestamps_ms = _read_timestamps_ms(video_path, camera_index)

    # ── Pass 1: per-frame face-ROI extraction (the expensive MediaPipe pass) ──
    frame_records: list[dict] = []  # for the "frames" JSON block (debug overlay)
    all_timestamps_ms: list[float] = []  # every processed frame, real or synthesized
    sample_timestamps_ms: list[float] = []  # only frames with a detected face
    sample_rgb: list[tuple[float, float, float]] = []
    # Each half of the face separately (nan when a half was empty), for the
    # beat-timing noise estimate in rppg.hrv.
    sample_halves: list[tuple] = []

    # A real timestamps_camN.csv exists only inside a recorded session's
    # video/ folder — standalone --video mode (and any session missing the
    # file) has none. Falling back to a flat 0ms for every frame (matching
    # sibling scripts' own fallback, which is harmless for THEIR purposes)
    # would collapse this plugin's entire window-building loop to zero
    # windows, since it needs real per-frame spacing to know how long the
    # recording spans — synthesizing from the video's own nominal fps is a
    # much more useful approximation (assumes constant frame timing, the
    # best available without real hardware timestamps) than a flat zero.
    have_real_timestamps = len(timestamps_ms) > 0

    frame_idx = 0
    t_start = time.perf_counter()
    while True:
        ok, frame = cap.read()
        if not ok:
            break

        if have_real_timestamps:
            ts_ms = (
                timestamps_ms[frame_idx] if frame_idx < len(timestamps_ms) else timestamps_ms[-1]
            )
        else:
            ts_ms = frame_idx / fps * 1000.0
        all_timestamps_ms.append(ts_ms)
        sample = extractor.extract(frame)
        if sample is not None:
            frame_records.append(
                {
                    "frame_index": frame_idx,
                    "timestamp_ms": round(ts_ms),
                    "face_detected": True,
                    "roi_bbox_px": list(sample.roi_bbox_px),
                }
            )
            sample_timestamps_ms.append(ts_ms)
            sample_rgb.append(sample.rgb_mean)
            sample_halves.append(sample.rgb_halves or ((np.nan,) * 3, (np.nan,) * 3))
        else:
            frame_records.append(
                {
                    "frame_index": frame_idx,
                    "timestamp_ms": round(ts_ms),
                    "face_detected": False,
                    "roi_bbox_px": None,
                }
            )

        frame_idx += 1
        if frame_idx % progress_interval == 0:
            elapsed = time.perf_counter() - t_start
            pct = frame_idx / max(total, 1) * 100
            print(f"  {pct:5.1f}%  ({frame_idx}/{total})  {elapsed:.1f}s elapsed", flush=True)

    cap.release()
    elapsed = time.perf_counter() - t_start
    print(
        f"[run_rppg] Done. {frame_idx} frames in {elapsed:.1f}s "
        f"({frame_idx / max(elapsed, 1e-9):.1f} fps throughput)",
        flush=True,
    )

    # ── Pass 2: sliding-window HR extraction (fast, pure numpy) ───────────────
    windows = _compute_windows(
        sample_timestamps_ms, sample_rgb, all_timestamps_ms, fps, backend, window_sec, hop_sec
    )

    raw_bpm = np.array([w["bpm"] if w["bpm"] is not None else np.nan for w in windows])
    smoothed = median_smooth(raw_bpm, smoothing_windows)
    for w, s in zip(windows, smoothed, strict=False):
        w["smoothed_bpm"] = None if np.isnan(s) else round(float(s), 1)

    try:
        beat_doc = _beats_and_hrv(
            video_path,
            sample_timestamps_ms,
            sample_rgb,
            sample_halves,
            all_timestamps_ms,
            fps,
            output_dir,
        )
    except Exception as exc:  # noqa: BLE001 - the windowed heart rate must still be written
        print(f"[run_rppg] Beat/HRV analysis failed ({exc}); writing heart rate only.", flush=True)
        beat_doc = {"hrv": None, "hrv_withheld": [f"beat analysis failed: {exc}"]}

    _write_results(
        video_path,
        windows,
        frame_records,
        backend,
        window_sec,
        hop_sec,
        camera_index,
        output_dir,
        beat_doc,
    )


def _frame_rate(all_ts_ms: list[float], fallback: float) -> float:
    """The real frame rate, from the median interval between timestamps."""
    if len(all_ts_ms) < 3:
        return fallback
    step = float(np.median(np.diff(np.asarray(all_ts_ms, dtype=np.float64))))
    return 1000.0 / step if step > 0 else fallback


def _conversation_states(session_dir: Path | None, video_path: Path):
    """Speaking and listening intervals (elapsed ms) of the person on this
    camera, from Conversation Timing's output, or ``None``."""
    if session_dir is None:
        return None
    path = session_dir / "conversation" / f"{video_path.stem}.conversation.json"
    if not path.exists():
        return None
    doc = json.loads(path.read_text(encoding="utf-8"))
    spurts = doc.get("spurts", [])
    return {
        "speaking": [(x["start_ms"], x["end_ms"]) for x in spurts if x["speaker"] == "subject"],
        "listening": [(x["start_ms"], x["end_ms"]) for x in spurts if x["speaker"] == "other"],
    }


def _beats_and_hrv(video_path, ts_ms, rgb, halves, all_ts_ms, fps, output_dir) -> dict:
    """Beats, intervals, HRV and their windows (rppg.hrv), as JSON parts."""
    if len(ts_ms) < 10:
        return {"hrv": None, "hrv_withheld": ["no face found"]}
    fs = _frame_rate(all_ts_ms, fps)
    times_s = np.asarray(ts_ms, dtype=np.float64) / 1000.0
    left = np.array([h[0] for h in halves], dtype=np.float64)
    right = np.array([h[1] for h in halves], dtype=np.float64)
    # The beat chain uses the equal-weight mean of the two halves, not the
    # ROI's pixel mean: the split-face noise estimate assumes the whole is
    # the average of the halves, and a turned head makes them unequal in
    # size. Frames without both halves fall back to the pixel mean.
    whole = np.asarray(rgb, dtype=np.float64)
    both = np.all(np.isfinite(left), axis=1) & np.all(np.isfinite(right), axis=1)
    whole = np.where(both[:, None], (left + right) / 2.0, whole)
    r = hrv.analyse(times_s, whole, fs, halves=(left, right))
    iv = r["ibi"]
    print(
        f"[run_rppg] Beats: {len(r['beats_s'])} found, {int(iv['nn'].sum())} clean intervals "
        f"({r['nn_seconds']:.0f} s), {100 * r['artifact_share']:.0f}% artifacts; "
        f"{r['timing_note']}",
        flush=True,
    )
    if r["hrv"] is None:
        print(f"[run_rppg] HRV not reported: {'; '.join(r['hrv_withheld'])}", flush=True)
    else:
        h = r["hrv"]
        print(
            f"[run_rppg] HRV: RMSSD {h.get('rmssd_ms')} ms (noise-corrected "
            f"{h.get('rmssd_corrected_ms')}), SDNN {h.get('sdnn_ms')} ms, "
            f"beat timing noise {h.get('timing_jitter_ms')} ms",
            flush=True,
        )
    doc = {
        "frame_rate": round(fs, 3),
        "beats": [
            {"t_ms": round(1000.0 * t, 1), "quality": round(float(q), 3)}
            for t, q in zip(r["beats_s"], r["beat_quality"], strict=True)
        ],
        "intervals": [
            {"t_ms": round(1000.0 * t, 1), "ibi_ms": round(1000.0 * v, 1), "nn": bool(n)}
            for t, v, n in zip(iv["t_s"], iv["ibi_s"], iv["nn"], strict=True)
        ],
        "hrv": r["hrv"],
        "hrv_withheld": r["hrv_withheld"],
        "hrv_windows": [
            {
                "start_ms": round(1000.0 * w["start_s"]),
                "end_ms": round(1000.0 * w["end_s"]),
                "hr_bpm": w["hr_bpm"],
                "rmssd_ms": w["rmssd_ms"],
                "nn_count": w["nn_count"],
            }
            for w in r["windows"]
        ],
    }
    states = _conversation_states(output_dir.parent if output_dir is not None else None, video_path)
    if states is not None:
        t_ms = 1000.0 * iv["t_s"]
        nn = iv["nn"]
        per = {}
        for name, spans in states.items():
            inside = np.zeros(t_ms.size, bool)
            for a, b in spans:
                inside |= (t_ms >= a) & (t_ms < b)
            v = iv["ibi_s"][nn & inside]
            per[name] = {
                "beats": int(v.size),
                "mean_hr_bpm": round(60.0 / float(v.mean()), 1) if v.size >= 10 else None,
            }
        doc["by_state"] = per
    return doc


def _compute_windows(
    sample_ts_ms: list[float],
    sample_rgb: list[tuple[float, float, float]],
    all_ts_ms: list[float],
    fps: float,
    backend: str,
    window_sec: float,
    hop_sec: float,
) -> list[dict]:
    if not all_ts_ms:
        return []

    combine_fn = BACKENDS[backend]
    ts = np.array(sample_ts_ms)
    rgb = np.array(sample_rgb) if sample_rgb else np.empty((0, 3))
    all_ts = np.array(all_ts_ms)

    window_ms = window_sec * 1000.0
    hop_ms = hop_sec * 1000.0
    end_ms = float(all_ts[-1])

    # Starts from the recording's own first processed-frame timestamp, not
    # a literal 0 — all_ts_ms comes straight from timestamps_camN.csv's
    # elapsed_ns column (app-launch-relative, per this project's
    # elapsed_ns() clock convention) whenever real hardware timestamps
    # exist, which is virtually always true for a real recorded session.
    # Starting at 0 produced dozens of guaranteed-empty leading windows on
    # a real session (confirmed: 30 of 44 windows for a video whose first
    # real frame landed at ~68s), which not only wasted a full pass of
    # this loop for no reason but also silently deflated pct_windows_good
    # in the summary — a session with perfect tracking for its entire real
    # duration would still report a low "good" percentage, since most of
    # the counted "windows" never had a chance to contain a single frame.
    windows: list[dict] = []
    start_ms = float(all_ts[0])
    while start_ms < end_ms:
        win_end_ms = start_ms + window_ms

        # Both timestamp arrays are already ascending (frames are processed
        # in sequential order, real or synthesized) — searchsorted finds
        # each window's [start_ms, win_end_ms) slice in O(log n) instead of
        # the O(n) full-array boolean mask this replaced, which made this
        # "fast, pure numpy" pass actually O(windows * total_frames) on a
        # long session (caught in code review).
        total_lo = int(np.searchsorted(all_ts, start_ms, side="left"))
        total_hi = int(np.searchsorted(all_ts, win_end_ms, side="left"))
        total_in_window = total_hi - total_lo

        lo = int(np.searchsorted(ts, start_ms, side="left"))
        hi = int(np.searchsorted(ts, win_end_ms, side="left"))
        valid_count = hi - lo
        valid_fraction = (valid_count / total_in_window) if total_in_window > 0 else 0.0

        bpm = None
        snr_db = None
        if valid_fraction >= MIN_VALID_FRACTION and valid_count >= 4:
            window_ts = ts[lo:hi]
            window_rgb = rgb[lo:hi]
            # Effective sample rate over the samples actually captured in
            # this window, rather than a literal uniform-grid interpolation
            # — a deliberate, documented simplification: the >=60%-density
            # gate above already ensures reasonably dense, evenly-spread
            # samples for typical short (<=0.5s) face-detection gaps in
            # well-lit indoor video, and Welch's periodogram already
            # tolerates mild sample-rate irregularity. A literal
            # interpolate-short-gaps-onto-a-fixed-grid version is a valid
            # future refinement, not attempted here.
            span_sec = (window_ts[-1] - window_ts[0]) / 1000.0
            eff_fs = (len(window_ts) - 1) / span_sec if span_sec > 0 else fps

            try:
                pulse = combine_fn(window_rgb)
                filtered = bandpass_filter(pulse, eff_fs, LOW_HZ, HIGH_HZ)
                bpm, snr_db = estimate_hr_welch(filtered, eff_fs, LOW_HZ, HIGH_HZ)
            except ValueError:
                # degenerate window (e.g. all-black ROI) — skip, don't crash
                bpm, snr_db = None, None

        windows.append(
            {
                "start_ms": round(start_ms),
                "end_ms": round(win_end_ms),
                "bpm": round(bpm, 1) if bpm is not None else None,
                "snr_db": round(snr_db, 1) if snr_db is not None else None,
                "valid_frame_fraction": round(valid_fraction, 3),
            }
        )
        start_ms += hop_ms

    return windows


def _write_results(
    video_path: Path,
    windows: list[dict],
    frames: list[dict],
    backend: str,
    window_sec: float,
    hop_sec: float,
    camera_index: int,
    output_dir: Path | None = None,
    beat_doc: dict | None = None,
) -> None:
    if output_dir is not None:
        output_dir.mkdir(parents=True, exist_ok=True)
        out_path = output_dir / f"{video_path.stem}.{backend}.rppg.json"
    else:
        # Standalone --video CLI mode has no session/rppg-folder concept to
        # hang a subfolder off — beside-the-video, matching sibling scripts'
        # identical fallback.
        out_path = video_path.with_name(f"{video_path.stem}.{backend}.rppg.json")

    valid_bpms = [w["bpm"] for w in windows if w["bpm"] is not None]
    summary = {
        "mean_bpm": round(float(np.mean(valid_bpms)), 1) if valid_bpms else None,
        "median_bpm": round(float(np.median(valid_bpms)), 1) if valid_bpms else None,
        "min_bpm": round(float(np.min(valid_bpms)), 1) if valid_bpms else None,
        "max_bpm": round(float(np.max(valid_bpms)), 1) if valid_bpms else None,
        "pct_windows_good": round(len(valid_bpms) / len(windows), 3) if windows else 0.0,
    }

    doc = {
        "schema": "mosaic-rppg-v2",
        "source_video": video_path.name,
        "backend": backend,
        "window_sec": window_sec,
        "hop_sec": hop_sec,
        "camera_index": camera_index,
        "windows": windows,
        "frames": frames,
        "summary": summary,
        # v2: beat-to-beat timing and heart-rate variability (rppg.hrv).
        **(beat_doc or {}),
    }
    out_path.write_text(json.dumps(doc, indent=2))
    print(f"[run_rppg] Heart-rate data → {out_path}", flush=True)


# ── Entry point ───────────────────────────────────────────────────────────────


def main() -> None:
    args = parse_args()

    if args.session:
        process_session(
            Path(args.session),
            args.backend,
            args.window_sec,
            args.hop_sec,
            args.smoothing_windows,
            args.min_confidence,
        )
    elif args.video:
        extractor = MediaPipeFaceRoiExtractor(min_confidence=args.min_confidence)
        video_path = Path(args.video)
        process_video(
            video_path,
            extractor,
            args.backend,
            args.window_sec,
            args.hop_sec,
            args.smoothing_windows,
            camera_index=_camera_index_from_filename(video_path),
        )


if __name__ == "__main__":
    main()
