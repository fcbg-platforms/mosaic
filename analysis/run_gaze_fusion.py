"""
MOSAIC Multi-Camera Gaze Fusion: where every subject looks, in 3D.

Usage::

    python run_gaze_fusion.py --session DIR [--skip 2] [--subjects N]
                              [--min-cameras 1] [--min-confidence 0.6]
                              [--camera N] [--raw] [--no-render] [--refresh]

Pipeline (see :mod:`gaze` and ``docs/math/gaze_fusion.rst``):

1. **Timeline**: ``synced/`` videos when Frame Sync Repair has made them,
   else raw ``video/`` with ``sync_manifest.json`` (:mod:`gaze.timeline`).
2. **Pass A**, one process per camera: faces (YuNet) and landmarks
   (MediaPipe on crops) on every ``--skip``-th tick, cached in
   ``gaze_fusion/landmarks_camN.npz``. A re-run with the same settings reuses
   the cache, so changing targets or subject names only costs the fusion and
   the videos.
3. **Fusion**: subjects across cameras, metric head pose, eyeball model,
   robust fused gaze, tracking and naming, smoothing, targets.
4. **Outputs**: ``gaze_fusion.json`` (read by the Analysis tab),
   ``gaze_fusion/gaze_fusion.csv`` and ``summary.json``.
5. **Pass B**: annotated video per camera, top-down room video, heat maps.

Progress lines follow the format the MOSAIC Analysis tab parses:
``[run_gaze_fusion] Camera i/n: ...`` and ``  NN.N%  (done/total)``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import multiprocessing as mp
import os
import sys
import time
from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, wait
from concurrent.futures.process import BrokenProcessPool
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))

from gaze.canonical import load_canonical_face  # noqa: E402
from gaze.face_detect import landmark_ids, landmarker_path  # noqa: E402
from gaze.fusion import (  # noqa: E402
    FaceObs,
    GazeRig,
    apply_subject_scale,
    assign_subjects,
    drop_duplicates,
    finalize,
)
from gaze.io_v2 import load_targets, summarise, write_csv, write_results  # noqa: E402
from gaze.timeline import load_timeline  # noqa: E402

TAG = "[run_gaze_fusion]"
CACHE_VERSION = 2


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="MOSAIC multi-camera 3D gaze")
    p.add_argument("--session", required=True, metavar="DIR", help="Recorded session directory")
    p.add_argument("--skip", type=int, default=2, help="Analyse every Nth tick (default 2)")
    p.add_argument("--subjects", type=int, default=None, help="Keep at most N subjects")
    p.add_argument(
        "--min-cameras",
        type=int,
        default=1,
        help="Cameras needed before a gaze is reported (default 1)",
    )
    p.add_argument(
        "--min-confidence", type=float, default=0.6, help="Face detector threshold (default 0.6)"
    )
    p.add_argument(
        "--camera",
        type=int,
        default=None,
        help="Use only this camera (1-based), e.g. an uncalibrated room or interview mode",
    )
    p.add_argument("--raw", action="store_true", help="Ignore synced/ and use the raw videos")
    p.add_argument("--no-render", action="store_true", help="Skip the videos and heat maps")
    p.add_argument("--refresh", action="store_true", help="Ignore cached landmarks")
    p.add_argument("--workers", type=int, default=None, help="Processes for detection/rendering")
    return p.parse_args(argv)


def log(msg: str) -> None:
    print(f"{TAG} {msg}", flush=True)


def fail(msg: str) -> None:
    print(f"{TAG} {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


# ── Cameras ──────────────────────────────────────────────────────────────────


def resolve_cameras(meta: dict, timeline, only: int | None) -> dict:
    """Calibrated cameras usable for this run, keyed by index.

    A camera needs intrinsics. With room (extrinsic) calibration every
    calibrated camera is used in the room frame. Without it, a single camera
    can still be used on its own, its frame then being the room frame.
    """
    from pose3d.triangulation import CameraGeom

    entries = {int(c.get("index", -1)): c for c in meta.get("cameras", [])}
    usable, with_rt = {}, {}
    for idx in sorted(timeline.cameras):
        if only is not None and idx != only:
            continue
        cam = entries.get(idx, {})
        cal = cam.get("calibration") or {}
        if not cal.get("calibrated") or not cal.get("camera_matrix") or not cal.get("dist_coeffs"):
            continue
        k = adjust_for_crop(
            np.array(cal["camera_matrix"], dtype=np.float64).reshape(3, 3), cal, cam
        )
        usable[idx] = (k, np.array(cal["dist_coeffs"], dtype=np.float64))
        if cal.get("extrinsic_calibrated") and cal.get("extrinsic_rt"):
            with_rt[idx] = np.array(cal["extrinsic_rt"], dtype=np.float64).reshape(4, 4)

    if with_rt:
        missing = sorted(set(usable) - set(with_rt))
        if missing:
            log(
                "No room calibration for "
                + ", ".join(f"Camera {i + 1}" for i in missing)
                + "; not used."
            )
        return {i: CameraGeom(i, usable[i][0], usable[i][1], with_rt[i]) for i in sorted(with_rt)}
    if len(usable) == 1:
        (idx,) = usable
        log(
            f"No room calibration: using Camera {idx + 1} on its own (its frame is the room frame)."
        )
        return {idx: CameraGeom(idx, usable[idx][0], usable[idx][1], np.eye(4))}
    return {}


def adjust_for_crop(k: np.ndarray, cal: dict, cam: dict) -> np.ndarray:
    """Shift the principal point when the recording used a different region
    of the sensor than the calibration (interview mode crops)."""
    rec_x, rec_y = cam.get("offset_x"), cam.get("offset_y")
    if rec_x is None or rec_y is None:
        return k
    cal_x, cal_y = cal.get("offset_x"), cal.get("offset_y")
    if cal_x is None or cal_y is None or cal_x < 0 or cal_y < 0:  # -1: not recorded
        if rec_x or rec_y:
            log(
                f"Camera {cam.get('index', 0) + 1}: recorded with a crop at ({rec_x}, {rec_y}) but "
                "its calibration does not say which region it used; assuming the full sensor."
            )
        cal_x, cal_y = 0, 0
    out = k.copy()
    out[0, 2] -= float(rec_x) - float(cal_x)
    out[1, 2] -= float(rec_y) - float(cal_y)
    return out


# ── Pass A: landmarks per camera ─────────────────────────────────────────────


def _cache_key(track, ids, skip: int, min_score: float) -> dict:
    """What the landmark cache depends on: the video file, which of its frames
    are analysed for which tick (a regenerated sync manifest or repair map
    changes that), and the detection settings."""
    st = track.video_path.stat()
    frames = hashlib.sha1(
        np.ascontiguousarray(track.frame_index).tobytes()
        + np.ascontiguousarray(track.missing).tobytes()
    ).hexdigest()
    return {
        "version": CACHE_VERSION,
        "video": str(track.video_path.name),
        "size": st.st_size,
        "mtime": int(st.st_mtime),
        "frames": frames,
        "skip": skip,
        "min_score": round(min_score, 3),
        "ids": [int(i) for i in ids],
    }


def detect_camera(args) -> tuple[int, str | None]:
    """Worker: faces and landmarks for one camera's analysed ticks.

    Returns ``(camera, note)``: ``note`` says the video ended before the
    timeline did (the cache then covers only what was read and is not marked
    reusable). Errors are raised, and reach the parent through the executor.
    """
    cam_index, video_path, frame_index, analyse, ids, min_score, cache_path, progress = args
    import cv2
    from gaze.face_detect import FaceFinder

    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        raise RuntimeError(f"cannot open {video_path}")
    finder = FaceFinder(ids, min_score=min_score)
    ticks, boxes, scores, points = [], [], [], []
    pos = -1
    frame = None
    note = None
    wanted = np.flatnonzero(analyse)
    try:
        for n, tick in enumerate(wanted):
            target = int(frame_index[tick])
            while pos < target:
                if not cap.grab():
                    break
                pos += 1
                frame = None
            if pos != target:
                note = (
                    f"the video ended at frame {pos + 1}, before tick {int(tick)} (frame {target})"
                )
                break
            if frame is None:
                ok, frame = cap.retrieve()
                if not ok:
                    frame = None
                    continue
            for face in finder.find(frame):
                ticks.append(int(tick))
                boxes.append(face.box)
                scores.append(face.score)
                points.append(face.points.astype(np.float32))
            if progress is not None and (n % 20 == 0 or n == len(wanted) - 1):
                progress.put((cam_index, n + 1, len(wanted)))
    finally:
        cap.release()
        finder.close()
    tmp = Path(str(cache_path) + ".part.npz")
    np.savez_compressed(
        tmp,
        ticks=np.array(ticks, dtype=np.int64),
        boxes=np.array(boxes, dtype=np.float32).reshape(-1, 4),
        scores=np.array(scores, dtype=np.float32),
        points=np.array(points, dtype=np.float32).reshape(-1, len(ids), 2),
    )
    tmp.replace(cache_path)
    return cam_index, note


def _workers(args, n_jobs: int) -> int:
    return args.workers or max(1, min(n_jobs, (os.cpu_count() or 2) // 2))


def run_pass_a(out_dir: Path, timeline, cameras, ids, args) -> dict:
    """Landmarks for every camera (from cache when unchanged); returns
    ``{camera: cache path}``.

    A worker that raises, or dies outright (a native crash in MediaPipe or
    OpenCV), ends the run with a clear message instead of leaving it waiting.
    """
    skip = max(1, args.skip)
    analyse_tick = np.zeros(timeline.n_ticks, dtype=bool)
    analyse_tick[::skip] = True
    jobs, caches = {}, {}
    for idx in sorted(cameras):
        track = timeline.cameras[idx]
        cache = out_dir / f"landmarks_cam{idx}.npz"
        key_path = out_dir / f"landmarks_cam{idx}.json"
        key = _cache_key(track, ids, skip, args.min_confidence)
        caches[idx] = cache
        if not args.refresh and cache.exists() and key_path.exists():
            try:
                if json.loads(key_path.read_text(encoding="utf-8")) == key:
                    log(f"Camera {idx + 1}: reusing cached landmarks.")
                    continue
            except (OSError, json.JSONDecodeError):
                pass
        key_path.unlink(missing_ok=True)  # only a complete run makes the cache valid
        jobs[idx] = {
            "track": track,
            "analyse": analyse_tick & ~track.missing & (track.frame_index >= 0),
            "cache": cache,
            "key": key,
            "key_path": key_path,
        }
    if not jobs:
        return caches

    workers = _workers(args, len(jobs))
    log(f"Finding faces in {len(jobs)} camera(s) with {workers} process(es)...")
    done = dict.fromkeys(jobs, 0)
    total = {idx: max(1, int(j["analyse"].sum())) for idx, j in jobs.items()}
    t0 = time.perf_counter()
    with mp.Manager() as manager, ProcessPoolExecutor(max_workers=workers) as pool:
        progress = manager.Queue()
        pending = {
            pool.submit(
                detect_camera,
                (
                    idx,
                    j["track"].video_path,
                    j["track"].frame_index,
                    j["analyse"],
                    ids,
                    args.min_confidence,
                    j["cache"],
                    progress,
                ),
            ): idx
            for idx, j in jobs.items()
        }
        finished, last = 0, -1.0
        while pending:
            ready, _ = wait(pending, timeout=0.5, return_when=FIRST_COMPLETED)
            while not progress.empty():
                cam, n, _total = progress.get()
                done[cam] = n
            pct = 100.0 * sum(done.values()) / sum(total.values())
            if pct - last >= 1.0:
                print(f"  {pct:5.1f}%  ({sum(done.values())}/{sum(total.values())})", flush=True)
                last = pct
            for fut in ready:
                cam = pending.pop(fut)
                try:
                    _cam, note = fut.result()
                except BrokenProcessPool:
                    fail(f"Camera {cam + 1}: the face-finding process crashed.")
                except Exception as e:  # noqa: BLE001 - reported, then the run stops
                    fail(f"Camera {cam + 1}: {e}")
                finished += 1
                elapsed = time.perf_counter() - t0
                if note:
                    log(f"Camera {cam + 1}: {note}; its later ticks have no faces.")
                else:
                    jobs[cam]["key_path"].write_text(json.dumps(jobs[cam]["key"]), encoding="utf-8")
                log(f"Camera {finished}/{len(jobs)}: faces found, video_{cam} ({elapsed:.0f} s)")
    return caches


def load_faces(caches: dict) -> dict:
    """``{tick: {camera: [FaceObs, ...]}}`` from the landmark caches."""
    faces: dict = {}
    for cam, path in caches.items():
        with np.load(path) as z:
            for tick, box, score, pts in zip(
                z["ticks"], z["boxes"], z["scores"], z["points"], strict=True
            ):
                faces.setdefault(int(tick), {}).setdefault(cam, []).append(
                    FaceObs(
                        camera=cam,
                        box=box.astype(np.float64),
                        points=pts.astype(np.float64),
                        score=float(score),
                    )
                )
    return faces


# ── Fusion ───────────────────────────────────────────────────────────────────


def fuse(rig: GazeRig, faces: dict, timeline, args, plane, regions):
    times_s = (timeline.times_ns - timeline.times_ns[0]) / 1e9
    ticks = sorted(faces)
    entries = []
    log(f"Fusing {len(ticks)} analysed tick(s) across cameras...")
    for n, tick in enumerate(ticks):
        for group in rig.associate(faces[tick]):
            entries.extend(rig.head_pose(group, tick))
        if n % max(1, len(ticks) // 50) == 0:
            print(f"  {100.0 * (n + 1) / len(ticks):5.1f}%  ({n + 1}/{len(ticks)})", flush=True)

    subjects = assign_subjects(entries, times_s, max_subjects=args.subjects)
    apply_subject_scale(entries, rig.cameras)
    for e in entries:
        if e.subject is not None:
            rig.compute_gaze(e)
            if e.n_cameras < args.min_cameras:
                # Below the requested cameras: no gaze, and no numbers about one.
                e.direction = None
                e.confidence = 0.0
                e.uncertainty_deg = None
                e.dispersion_deg = None
                for c in e.per_camera:
                    c.direction = None
    entries = drop_duplicates(entries)
    finalize(entries, subjects, times_s, regions=regions, plane=plane)
    return entries, subjects


# ── Main ─────────────────────────────────────────────────────────────────────


def main(argv=None) -> None:
    args = parse_args(argv)
    session = Path(args.session)
    meta_path = session / "session_meta.json"
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        fail(f"Cannot read {meta_path}")

    timeline = load_timeline(session, prefer_synced=not args.raw)
    if timeline is None:
        fail(
            "No synced/ videos (Frame Sync Repair) and no sync_manifest.json: nothing tells "
            "which frames belong together. Run Frame Sync Repair first."
        )
    log(
        f"Timeline: {timeline.source}, {timeline.n_ticks} ticks at {timeline.fps:.2f} fps, "
        f"cameras {', '.join(str(i + 1) for i in sorted(timeline.cameras))}"
    )

    if args.camera is not None and args.camera < 1:
        fail("--camera is 1-based: Camera 1 is --camera 1.")
    only = args.camera - 1 if args.camera is not None else None
    cameras = resolve_cameras(meta, timeline, only)
    if not cameras:
        fail(
            "No usable camera: gaze needs each camera's intrinsic calibration, and room "
            "calibration to combine several cameras (or --camera N to use one on its own)."
        )
    plane, regions, names = load_targets(session, meta)

    task = landmarker_path()
    canonical = load_canonical_face(task)
    ids = landmark_ids(canonical.rigid_ids)
    out_dir = session / "gaze_fusion"
    out_dir.mkdir(exist_ok=True)

    caches = run_pass_a(out_dir, timeline, cameras, ids, args)
    faces = load_faces(caches)
    rig = GazeRig(cameras, canonical, ids)
    entries, subjects = fuse(rig, faces, timeline, args, plane, regions)
    if not subjects:
        log("No subject was seen long enough to report.")

    skip = max(1, args.skip)
    analysed = list(range(0, timeline.n_ticks, skip))
    videos = {i: f"gaze_fusion/video_{i}.gaze.mp4" for i in cameras} if not args.no_render else {}
    room_video = "gaze_fusion/room_topdown.mp4" if not args.no_render else None
    out = write_results(
        session,
        timeline,
        entries,
        analysed,
        subjects,
        names,
        cameras,
        plane,
        regions,
        skip,
        videos,
        room_video,
    )
    write_csv(out_dir / "gaze_fusion.csv", timeline, entries, names)
    tick_s = 1.0 / timeline.fps
    summary = summarise(entries, subjects, names, tick_s, timeline.n_ticks, skip)
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    log(f"{len(subjects)} subject(s), {len(entries)} estimate(s) -> {out}")
    for name, s in summary["subjects"].items():
        top = ", ".join(f"{k} {v:.1f} s" for k, v in list(s["looked_at_s"].items())[:3])
        log(f"  {name}: seen {s['seen_s']:.1f} s; looked at {top or 'nothing recognised'}")

    if not args.no_render:
        written, room_ok = render(
            session,
            out_dir,
            timeline,
            cameras,
            entries,
            subjects,
            names,
            regions,
            plane,
            skip,
            args,
        )
        if written != set(videos) or not room_ok:
            # Point only at videos that exist, so the Analysis tab never
            # offers one that is missing.
            write_results(
                session,
                timeline,
                entries,
                analysed,
                subjects,
                names,
                cameras,
                plane,
                regions,
                skip,
                {i: videos[i] for i in written},
                room_video if room_ok else None,
            )
    log("Done.")


def render(
    session, out_dir, timeline, cameras, entries, subjects, names, regions, plane, skip, args
):
    """Pass B. Returns ``(cameras whose video was written, room video written)``;
    a failure in one video is reported and does not stop the others."""
    from gaze.render import build_render_data, render_camera_video, render_topdown, write_heatmaps

    data = build_render_data(entries, subjects, names, timeline, cameras, regions, plane, skip)
    jobs = [
        (
            i,
            timeline.cameras[i].video_path,
            timeline.cameras[i].frame_index,
            out_dir / f"video_{i}.gaze.mp4",
            data,
        )
        for i in sorted(cameras)
    ]
    log(f"Writing {len(jobs)} annotated video(s) and the room view...")
    written, room_ok = set(), False
    with ProcessPoolExecutor(max_workers=_workers(args, len(jobs) + 1)) as pool:
        room = pool.submit(render_topdown, out_dir / "room_topdown.mp4", data)
        futures = {pool.submit(render_camera_video, job): job[0] for job in jobs}
        n = 0
        for fut in list(futures):
            cam = futures[fut]
            n += 1
            try:
                _cam, err = fut.result()
            except Exception as e:  # noqa: BLE001 - one video failing must not stop the rest
                err = str(e) or type(e).__name__
            if err:
                log(f"Camera {cam + 1}: video not written: {err}")
            else:
                written.add(cam)
                log(f"Camera {n}/{len(jobs)}: wrote gaze_fusion/video_{cam}.gaze.mp4")
            print(f"  {100.0 * n / len(jobs):5.1f}%  ({n}/{len(jobs)})", flush=True)
        try:
            room.result()
            room_ok = True
        except Exception as e:  # noqa: BLE001
            log(f"Room view not written: {e}")
    for path in write_heatmaps(out_dir, data):
        log(f"Heat map: {path.relative_to(session).as_posix()}")
    return written, room_ok


if __name__ == "__main__":
    mp.freeze_support()
    main()
