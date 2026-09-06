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
from facemask.masking import (
    apply_mask,
    apply_mask_region,
    dilate_mask,
    expand_and_clip,
    rasterize_boxes,
)
from facemask.segmenters import DEFAULT_SEG_CONF, DEFAULT_SEG_MODEL, make_segmenter

_MARGIN_FRAC = 0.25  # padding around each detected face box; not CLI-exposed

# Whole-body dilation, as a fraction of FRAME height — deliberately not a
# fraction of the person's own size the way _MARGIN_FRAC is.
#
# The dominant silhouette-boundary error is the segmentation prototype's
# quantization, roughly 4 * frame_h / imgsz pixels (~11 px at 1080p), and that
# is a constant in frame pixels regardless of how large the person is. A
# proportional rule would hand a distant 100-px-tall person about 1.5 px of
# slack against an 11-px error — backwards, since distant people are exactly
# where the segmenter is least reliable.
_SEG_MARGIN_FRAC = 0.015
_SEG_MARGIN_MIN_PX = 4

# What counts as "this run is systematically broken" rather than "a few frames
# had trouble". Both bars must be cleared before the output is discarded: a
# model that fails to load onto the GPU throws on essentially every frame,
# whereas a couple of transient failures still leave a perfectly usable
# anonymized copy. Session videos here are often only ~141 frames, so a bare
# percentage would throw away good output over two bad frames.
_MAX_ERROR_FRAME_FRAC = 0.5
_MIN_ERROR_FRAMES_TO_FAIL = 5

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
    parser.add_argument(
        "--region",
        choices=["face", "body"],
        default="face",
        help="What to mask: just faces, or each person's whole silhouette "
        "(default: face). Whole-body masks the union of person segmentation "
        "and the face detector, so a person the segmenter misses still has "
        "their face covered.",
    )
    parser.add_argument(
        "--seg-model",
        default=DEFAULT_SEG_MODEL,
        help=f"Person-segmentation checkpoint for --region body (default: {DEFAULT_SEG_MODEL})",
    )
    parser.add_argument(
        "--seg-conf",
        type=float,
        default=DEFAULT_SEG_CONF,
        help="Segmentation confidence threshold for --region body "
        f"(default: {DEFAULT_SEG_CONF}). Deliberately lower than --conf: the "
        "face-box union backstops recall, so the segmenter should be biased "
        "toward finding people rather than being sure about them.",
    )
    parser.add_argument(
        "--seg-imgsz",
        type=int,
        default=640,
        help="Segmentation inference resolution for --region body (default: 640). "
        "Larger resolves small/distant people better, at proportionally more time.",
    )
    return parser.parse_args()


def effective_skip(region: str, skip: int) -> int:
    """Frame-skip actually safe to use for ``region``.

    Whole-body is forced to 1. ``--skip N`` reuses the previous detection on
    the frames in between, which faces tolerate because expand_and_clip() pads
    every box by 25% of its own size — tens of pixels of slack. A segmentation
    mask hugs the silhouette and has only its dilation, so a stale mask
    misaligns as the person moves and leaves a crescent of them unmasked. The
    failure is silent and pretty: a perfectly plausible person-shaped blur,
    standing next to a partly-exposed person.

    Clamps rather than errors, so a scripted ``--skip 3 --region body`` still
    produces a correct video, just more slowly. Clamping is the fail-safe
    direction; honouring the request would be fail-open.
    """
    if region == "body" and skip != 1:
        print(
            f"[run_face_mask] --skip {skip} is unsafe with --region body (a reused "
            f"silhouette misaligns as people move, leaving them partly unmasked) "
            f"— forcing --skip 1.",
            file=sys.stderr,
            flush=True,
        )
        return 1
    return max(1, skip)


def output_name(video_path: Path, region: str, backend: str) -> str:
    """Filename for an anonymized copy, namespaced by what it actually covers.

    Region and backend both change *which pixels are masked*, so two runs that
    differ in either must not overwrite each other — a user must never be
    holding a face-masked file believing it is body-masked. Style is
    deliberately not in the name: blur vs solid box changes appearance, not
    coverage, and is obvious at a glance.

    Must stay in lockstep with AnalysisTabW::anonymized_video_path_for().
    """
    return f"{video_path.stem}.{region}.{backend}{video_path.suffix}"


def partial_path_for(out_path: Path) -> Path:
    """In-progress name for ``out_path``, published by rename on success.

    The marker goes in the stem, never the extension: OpenCV picks its muxer
    from the filename suffix, so a ``.mp4.partial`` name matches no muxer and
    ``cv2.VideoWriter`` silently refuses to open at all — which would break
    every run rather than protect anything.
    """
    return out_path.with_name(out_path.stem + ".partial" + out_path.suffix)


def seg_dilate_radius(frame_h: int) -> int:
    """Dilation radius in pixels for a whole-body mask. See _SEG_MARGIN_FRAC."""
    return max(_SEG_MARGIN_MIN_PX, round(_SEG_MARGIN_FRAC * frame_h))


# ── Session mode ─────────────────────────────────────────────────────────────


def process_session(
    session_dir: Path,
    backend: str,
    model: str | None,
    style: str,
    conf: float,
    skip: int,
    device: str | None,
    region: str = "face",
    segmenter=None,
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
        out_path = out_dir / output_name(video_path, region, backend)
        if not process_video(
            video_path, detector, out_path, style, skip, region=region, segmenter=segmenter
        ):
            failures += 1

    if failures:
        print(
            f"[run_face_mask] {failures}/{len(videos)} video(s) failed to process "
            f"— see errors above.",
            file=sys.stderr,
        )
        sys.exit(1)


def process_video(
    video_path: Path,
    detector,
    out_path: Path,
    style: str,
    skip: int,
    region: str = "face",
    segmenter=None,
) -> bool:
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

    # Written to a .partial file and renamed only once the whole video has
    # been processed. Killed midway — the app closing, the user hitting Stop —
    # a direct write leaves a truncated file sitting at the real output path,
    # which the Analysis tab then reports as complete and plays back as if it
    # were. The suffix makes an interrupted run obvious instead.
    partial_path = partial_path_for(out_path)
    writer = _open_writer(partial_path, fps, (width, height))
    if writer is None:
        print(f"[run_face_mask] Could not open a video writer for {partial_path}", file=sys.stderr)
        partial_path.unlink(missing_ok=True)  # OpenCV may leave a 0-byte stub
        cap.release()
        return False

    skip = effective_skip(region, skip)
    dilate_radius = seg_dilate_radius(height)

    frame_idx = 0
    total_faces = 0
    error_frames = 0
    # Counted separately because they mean different things: an exception is
    # loud and blanks the frame, whereas the segmenter simply *missing*
    # somebody is silent and is precisely what the face-box union exists to
    # catch. A non-zero second number is the most actionable diagnostic this
    # plugin can produce.
    seg_miss_frames = 0
    last_boxes: list = []
    last_mask = None
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

                if region == "body":
                    person = dilate_mask(segmenter.segment(frame), dilate_radius)
                    if not person.any() and last_boxes:
                        seg_miss_frames += 1
                    # One composite, not a segmentation pass followed by a face
                    # pass: two passes would blur the overlap twice with
                    # different kernels, leaving a visible seam and making the
                    # result depend on the order they ran in.
                    last_mask = rasterize_boxes(person, last_boxes)

            if region == "body":
                if last_mask is None:
                    # Only reachable if a future change lets body runs skip
                    # frames and the very first segmentation threw. Raise into
                    # the handler below rather than write the frame through.
                    raise RuntimeError("no segmentation mask available for this frame")
                apply_mask_region(frame, last_mask, style)
            else:
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
        partial_path.unlink(missing_ok=True)
        return False

    # A run where nearly every frame errored produced an all-black video, not
    # an anonymized one. Reporting success would light the plugin's "already
    # run" indicator green over it.
    if error_frames > max(_MIN_ERROR_FRAMES_TO_FAIL, _MAX_ERROR_FRAME_FRAC * frame_idx):
        print(
            f"[run_face_mask] {video_path.name}: {error_frames}/{frame_idx} frames failed "
            f"and were blacked out — refusing to publish this as an anonymized copy.",
            file=sys.stderr,
        )
        partial_path.unlink(missing_ok=True)
        return False

    # Publish only now that the whole video is written.
    partial_path.replace(out_path)

    elapsed = time.perf_counter() - t_start
    suffix = f", {error_frames} frame(s) had errors (see above)" if error_frames else ""
    if seg_miss_frames:
        suffix += (
            f", {seg_miss_frames} frame(s) where segmentation found nobody but the face "
            f"detector did (those faces were still masked)"
        )
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

    # Built once, before any frame loop. Constructing it lazily inside
    # process_video() would reload the weights per camera, and would turn a
    # first-use download failure into thousands of blacked-out frames instead
    # of one clean error before anything is written.
    segmenter = None
    if args.region == "body":
        segmenter = make_segmenter(args.seg_model, args.seg_conf, args.device, imgsz=args.seg_imgsz)

    if args.session:
        process_session(
            Path(args.session),
            args.backend,
            args.model,
            args.style,
            args.conf,
            args.skip,
            args.device,
            region=args.region,
            segmenter=segmenter,
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
        out_path = session_root / "anonymized" / output_name(video_path, args.region, args.backend)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        if not process_video(
            video_path,
            detector,
            out_path,
            args.style,
            args.skip,
            region=args.region,
            segmenter=segmenter,
        ):
            sys.exit(1)


if __name__ == "__main__":
    main()
