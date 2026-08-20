"""
MOSAIC face-masking runner — anonymizes faces in recorded session videos for
external sharing. Never touches the original recordings: writes a blurred/
boxed copy of each video into a sibling "anonymized/" folder.

    python analysis/run_face_mask.py --session /path/to/session_2026-06-07_14-30
    python analysis/run_face_mask.py --video /path/to/video/cam0.mp4

See analysis/README.rst for full documentation.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import cv2
from facemask.detectors import make_detector
from facemask.masking import apply_mask, expand_and_clip

_MARGIN_FRAC = 0.25  # padding around each detected box; not CLI-exposed

# ── CLI ───────────────────────────────────────────────────────────────────────


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MOSAIC face-mask anonymizer")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--session", metavar="DIR", help="Anonymize all .mp4 files in a recorded session directory"
    )
    group.add_argument("--video", metavar="FILE", help="Anonymize a single video file")

    parser.add_argument(
        "--backend",
        choices=["mediapipe", "yolov8", "opencv"],
        default="mediapipe",
        help="Face-detection backend (default: mediapipe)",
    )
    parser.add_argument(
        "--model", default=None, help="Model override (only meaningful for --backend yolov8)"
    )
    parser.add_argument(
        "--style",
        choices=["blur", "box"],
        default="blur",
        help="Anonymization style (default: blur)",
    )
    parser.add_argument(
        "--conf", type=float, default=0.5, help="Detection confidence threshold (default: 0.5)"
    )
    parser.add_argument(
        "--skip",
        type=int,
        default=1,
        help="Run the detector every Nth frame, reusing the last "
        "detected boxes on skipped frames (default: 1 = every "
        "frame — raising this risks a skipped frame's fast "
        "head motion going unmasked)",
    )
    parser.add_argument(
        "--device", default=None, help="Inference device for the yolov8 backend: cpu / cuda:0"
    )
    return parser.parse_args()


# ── Session mode ─────────────────────────────────────────────────────────────


def process_session(
    session_dir: Path,
    backend: str,
    model: str | None,
    style: str,
    conf: float,
    skip: int,
    device: str | None,
) -> None:
    videos = sorted((session_dir / "video").glob("*.mp4"))
    if not videos:
        print(f"[run_face_mask] No .mp4 files found in {session_dir / 'video'}", file=sys.stderr)
        sys.exit(1)

    print(f"[run_face_mask] Found {len(videos)} video(s) in {session_dir / 'video'}", flush=True)
    detector = make_detector(backend, model, conf, device)
    out_dir = session_dir / "anonymized"
    out_dir.mkdir(parents=True, exist_ok=True)

    failures = 0
    # "Camera N/M:" banner — distinct, easily-regexed format matching
    # run_pose.py's own convention — drives AnalysisTabW's coarser,
    # session-wide progress bar (parsed alongside the finer per-frame one).
    for position, video_path in enumerate(videos, start=1):
        print(f"[run_face_mask] Camera {position}/{len(videos)}: {video_path.name}", flush=True)
        if not process_video(video_path, detector, out_dir / video_path.name, style, skip):
            failures += 1

    if failures:
        print(
            f"[run_face_mask] {failures}/{len(videos)} video(s) failed to process "
            f"— see errors above.",
            file=sys.stderr,
        )
        sys.exit(1)


def process_video(video_path: Path, detector, out_path: Path, style: str, skip: int) -> bool:
    """Returns True on success. On any failure, prints to stderr and removes
    a partial/empty output file rather than leaving one behind that looks
    like a successful anonymization but silently contains nothing."""
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        print(f"[run_face_mask] Cannot open {video_path}", file=sys.stderr)
        return False

    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    print(
        f"[run_face_mask] Processing {video_path.name}  ({total} frames @ {fps:.1f} fps)",
        flush=True,
    )

    # Scales to the video's own length instead of a fixed 100-frame stride
    # (matches run_pose.py's identical fix): short session videos (~141
    # frames is typical) get a progress update every frame instead of one
    # single jump near the end, while long videos are capped at ~200 total
    # prints. Consumed by AnalysisTabW's progress-bar regex, not dumped into
    # the visible log, so there's no "avoid spamming the log" reason to keep
    # the interval coarse.
    progress_interval = max(1, total // 200)

    writer = _open_writer(out_path, fps, (width, height))
    if writer is None:
        print(f"[run_face_mask] Could not open a video writer for {out_path}", file=sys.stderr)
        cap.release()
        return False

    frame_idx = 0
    total_faces = 0
    error_frames = 0
    last_boxes: list = []
    t_start = time.perf_counter()

    while True:
        ok, frame = cap.read()
        if not ok:
            break

        # A single frame's detection/masking can fail on a frame-content-
        # dependent edge case without meaning the whole video is
        # unrecoverable — catch here so one bad frame doesn't abort this
        # video (and, since process_session() calls this function once per
        # camera, doesn't abort the other cameras in the same session
        # either). apply_mask() modifies `frame` in place box-by-box, so an
        # exception partway through could leave some faces masked and
        # others not — rather than risk writing a partially- or
        # un-masked frame (a real privacy leak in a tool whose whole job is
        # anonymization), blank the entire frame to solid black on any
        # failure instead of trying to salvage whatever partial state it's
        # in.
        try:
            if frame_idx % skip == 0:
                raw_boxes = detector.detect(frame)
                last_boxes = [
                    expand_and_clip(box, _MARGIN_FRAC, width, height) for box in raw_boxes
                ]
                total_faces += len(last_boxes)

            apply_mask(frame, last_boxes, style)
        except Exception as exc:  # noqa: BLE001 — must not let one frame kill the run
            error_frames += 1
            frame[:] = 0
            print(
                f"[run_face_mask] {video_path.name}: frame {frame_idx} failed ({exc}) "
                f"— writing a blacked-out frame instead",
                file=sys.stderr,
                flush=True,
            )

        writer.write(frame)

        frame_idx += 1
        if frame_idx % progress_interval == 0:
            elapsed = time.perf_counter() - t_start
            pct = frame_idx / max(total, 1) * 100
            print(f"  {pct:5.1f}%  ({frame_idx}/{total})  {elapsed:.1f}s elapsed", flush=True)

    cap.release()
    writer.release()

    if frame_idx == 0:
        print(
            f"[run_face_mask] {video_path.name}: 0 frames read — removing empty output",
            file=sys.stderr,
        )
        out_path.unlink(missing_ok=True)
        return False

    elapsed = time.perf_counter() - t_start
    suffix = f", {error_frames} frame(s) had errors (see above)" if error_frames else ""
    print(
        f"[run_face_mask] Done. {frame_idx} frames, {total_faces} face detection(s) "
        f"in {elapsed:.1f}s → {out_path}{suffix}",
        flush=True,
    )
    return True


def _open_writer(out_path: Path, fps: float, size: tuple[int, int]):
    # avc1 (H.264) plays back more reliably in Qt/Windows Media Foundation
    # than mp4v, but isn't always available depending on the OpenCV build's
    # bundled FFmpeg — fall back to mp4v (always available) if it fails.
    for fourcc_name in ("avc1", "mp4v"):
        fourcc = cv2.VideoWriter_fourcc(*fourcc_name)
        writer = cv2.VideoWriter(str(out_path), fourcc, fps, size)
        if writer.isOpened():
            return writer
        writer.release()
    return None


# ── Entry point ───────────────────────────────────────────────────────────────


def main() -> None:
    args = parse_args()

    if args.session:
        process_session(
            Path(args.session),
            args.backend,
            args.model,
            args.style,
            args.conf,
            args.skip,
            args.device,
        )
    elif args.video:
        video_path = Path(args.video)
        detector = make_detector(args.backend, args.model, args.conf, args.device)
        # If the video lives in a session's video/ subfolder (the layout
        # MOSAIC recordings use), write into the sibling anonymized/ folder
        # at the session root — the same location --session mode uses —
        # so the app's anonymized_video_path_for() lookup finds it either
        # way. Otherwise fall back to a folder next to the video itself.
        session_root = (
            video_path.parent.parent if video_path.parent.name == "video" else video_path.parent
        )
        out_path = session_root / "anonymized" / video_path.name
        out_path.parent.mkdir(parents=True, exist_ok=True)
        if not process_video(video_path, detector, out_path, args.style, args.skip):
            sys.exit(1)


if __name__ == "__main__":
    main()
