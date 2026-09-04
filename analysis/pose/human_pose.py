"""
YOLOv8-pose backend for real-time human pose estimation.

Recommended model weights by speed/accuracy trade-off:
  yolov8n-pose.pt  — nano,   ~4 MB,  fastest,  good for CPU preview (≥15 fps)
  yolov8s-pose.pt  — small,  ~24 MB, balanced  (CPU ≥8 fps, GPU ≥60 fps)
  yolov8m-pose.pt  — medium, ~52 MB, accurate  (GPU recommended)
  yolov8l-pose.pt  — large,  ~87 MB, best      (GPU required)

Install: pip install ultralytics
"""

from __future__ import annotations

import time

import numpy as np

from .keypoints import PoseResult, SubjectPose

# ── lazy import: ultralytics is optional ─────────────────────────────────────
try:
    from ultralytics import YOLO as _YOLO

    _ULTRALYTICS_OK = True
except ImportError:
    _ULTRALYTICS_OK = False
    _YOLO = None  # type: ignore[assignment]


# Tracker name passed to ultralytics and recorded in the .pose.json header.
# BoT-SORT over ByteTrack because it adds camera-motion compensation and
# optional ReID on top of the same association core, and ships with
# ultralytics either way — no new dependency.
TRACKER_NAME = "botsort"
TRACKER_CONFIG = "botsort.yaml"

# Confidence floor handed to the tracker, as opposed to the one applied to
# what actually gets written. Matches botsort.yaml's track_low_thresh.
TRACK_INPUT_CONF = 0.1


def resolve_subject_ids(track_ids: list[int | None] | None, detection_count: int) -> list[int]:
    """Map one frame's ultralytics track ids onto MOSAIC subject ids.

    Parameters
    ----------
    track_ids : list of (int or None), or None
        ``res.boxes.id`` converted to plain ints, or ``None`` when the tracker
        produced nothing for this frame (it has no confirmed tracks yet).
    detection_count : int
        How many detections this frame actually has. The returned list is
        always exactly this long.

    Returns
    -------
    list of int
        A tracker id (always >= 1) is used as-is — it means "the same physical
        person" across frames, which is the entire point. Anything else falls
        back to ``-(i + 1)``: negative marks "not tracked in this frame", and
        it stays distinct per detection so two untracked people never collapse
        onto one id. Collapsing them would be worse than the bug this replaces,
        because the C++ side looks subjects up *by* id.

        Length mismatches are resolved per index rather than by zipping: a
        silent shift would attach one person's keypoints to another person's
        id, which is exactly the failure mode being eliminated.
    """
    if detection_count <= 0:
        return []
    ids = track_ids or []
    out: list[int] = []
    for i in range(detection_count):
        tid = ids[i] if i < len(ids) else None
        out.append(int(tid) if tid is not None and int(tid) > 0 else -(i + 1))
    return out


def _reset_model_trackers(model) -> bool:
    """Reset every tracker attached to a YOLO model's predictor.

    Returns ``False`` when there is nothing to reset — no ``track()`` call has
    happened yet, so no predictor/trackers exist. Split out from
    :meth:`HumanPoseEstimator.reset_tracker` so it can be unit-tested against a
    stub model, without ultralytics installed.
    """
    predictor = getattr(model, "predictor", None)
    trackers = getattr(predictor, "trackers", None) if predictor is not None else None
    if not trackers:
        return False
    for tracker in trackers:
        tracker.reset()
    return True


class HumanPoseEstimator:
    """Wraps YOLOv8-pose for single-frame inference.

    Parameters
    ----------
    model_name : str, default "yolov8n-pose.pt"
        YOLOv8 model variant. Downloaded automatically from the
        Ultralytics CDN on first use (~4-87 MB).
    device : str or None, default None
        Inference device — ``"cpu"``, ``"cuda:0"``, ``"mps"`` (Apple
        Silicon). ``None`` auto-detects (prefers CUDA → MPS → CPU).
    conf_threshold : float, default 0.40
        Minimum detection confidence to include a subject.
    iou_threshold : float, default 0.70
        NMS IoU threshold.

    Raises
    ------
    ImportError
        If ``ultralytics`` is not installed.
    """

    def __init__(
        self,
        model_name: str = "yolov8n-pose.pt",
        device: str | None = None,
        conf_threshold: float = 0.40,
        iou_threshold: float = 0.70,
    ) -> None:
        if not _ULTRALYTICS_OK:
            raise ImportError("ultralytics is not installed.  Run: pip install ultralytics")

        self._conf = conf_threshold
        self._iou = iou_threshold
        self._device = device or self._auto_device()

        print(f"[HumanPoseEstimator] Loading {model_name} on {self._device} …", flush=True)
        self._model = _YOLO(model_name)
        # Warm-up pass (avoids slow first real inference)
        dummy = np.zeros((320, 320, 3), dtype=np.uint8)
        self._model(dummy, verbose=False, device=self._device)
        print("[HumanPoseEstimator] Ready.", flush=True)

    # ── Public API ────────────────────────────────────────────────────────────

    def reset_tracker(self) -> None:
        """Drop all cross-frame tracking state, starting a fresh identity space.

        MUST be called at every video boundary. :meth:`infer` passes
        ``persist=True``, and ultralytics' own "switched videos" auto-reset
        never fires for us — it is short-circuited by ``persist=True``, and in
        any case keys off the source path, which is constant for the in-memory
        ndarray frames we pass. Without this, ``run_pose.py``'s session mode
        (one estimator threaded through every camera's video) would hand
        camera N's live tracks to camera N+1's opening frames and silently glue
        two different people onto one subject id.

        Resetting also restarts ids at 1 per video, which is what makes the
        "ids are per-video and never comparable across cameras" contract in the
        exported CSV true rather than aspirational.
        """
        if not _reset_model_trackers(self._model):
            # Nothing attached yet (no track() call so far), or a build that
            # doesn't expose .trackers. Dropping the predictor forces
            # ultralytics to rebuild it — and its trackers — on the next call.
            self._model.predictor = None

    def infer(
        self,
        frame: np.ndarray,
        frame_index: int = 0,
        timestamp_ns: int = 0,
        camera_index: int = 0,
    ) -> PoseResult:
        """Run pose estimation on one BGR frame.

        Parameters
        ----------
        frame : numpy.ndarray
            BGR frame, as returned by ``cv2.imread``/``cv2.VideoCapture``.
        frame_index : int, default 0
            Caller-supplied frame index, echoed into the result.
        timestamp_ns : int, default 0
            Caller-supplied timestamp (ns), echoed into the result.
        camera_index : int, default 0
            Caller-supplied camera index, echoed into the result.

        Returns
        -------
        PoseResult
            Structured result containing per-subject keypoints, echoing
            ``frame_index``/``timestamp_ns``/``camera_index`` back
            unchanged and reporting this call's own ``inference_ms``.
        """
        t0 = time.perf_counter()
        # track(), not a plain detection call: subject_id must mean "the same
        # physical person" across frames. persist=True keeps tracker state
        # between calls, which is why reset_tracker() has to run at every video
        # boundary (see its docstring).
        #
        # inference_ms now includes the tracker update, not just the network.
        results = self._model.track(
            frame,
            # Deliberately below self._conf. BoT-SORT's second association
            # stage — the one that keeps a track alive through a weak or
            # partly-occluded frame — can only work with detections the
            # detector was allowed to emit, and botsort.yaml's
            # track_low_thresh is 0.1. Passing our own 0.40 here would starve
            # it completely (ultralytics defaults conf to 0.1 for exactly this
            # reason, but only when the caller doesn't pass one).
            #
            # Which detections get *written* is unchanged — self._conf is
            # re-applied per detection below, and new_track_thresh still stops
            # weak detections from spawning their own tracks. What does change
            # is that NMS (iou=self._iou) now also runs across the 0.1-0.40
            # band, so a weak duplicate box of an already-tracked person can
            # survive it, take that person's track, and push the strong box
            # onto a fresh id — a spurious identity split in exactly the
            # crowded frames this is meant to help. Not asserted away: it is
            # on the room-11 verification list to check against real footage.
            conf=min(self._conf, TRACK_INPUT_CONF),
            iou=self._iou,
            device=self._device,
            persist=True,
            tracker=TRACKER_CONFIG,
            verbose=False,
        )
        inference_ms = (time.perf_counter() - t0) * 1000.0

        subjects: list[SubjectPose] = []
        for res in results:
            if res.keypoints is None:
                continue
            kpts_xy = res.keypoints.xy.cpu().numpy()  # (N, 17, 2)
            kpts_conf = res.keypoints.conf  # may be None
            boxes = res.boxes

            # track() pins batch=1, so `results` holds exactly one `res` and
            # the -(i+1) fallbacks below cannot collide across iterations.
            raw_ids: list[int] | None = None
            if boxes is not None and boxes.id is not None:
                raw_ids = [int(v) for v in boxes.id.int().cpu().tolist()]
            subject_ids = resolve_subject_ids(raw_ids, len(kpts_xy))

            for i, kxy in enumerate(kpts_xy):
                vis: list[float]
                if kpts_conf is not None:
                    vis = kpts_conf[i].cpu().numpy().tolist()
                else:
                    vis = [1.0] * kxy.shape[0]

                det_conf = float(boxes.conf[i].cpu()) if boxes is not None else 1.0
                # Re-apply the caller's real confidence floor, since the
                # tracker was deliberately fed a lower one above.
                if det_conf < self._conf:
                    continue
                bbox = (0.0, 0.0, 0.0, 0.0)
                if boxes is not None:
                    b = boxes.xyxy[i].cpu().numpy()
                    bbox = (float(b[0]), float(b[1]), float(b[2]), float(b[3]))

                subjects.append(
                    SubjectPose(
                        subject_id=subject_ids[i],
                        confidence=det_conf,
                        keypoints=[(float(pt[0]), float(pt[1])) for pt in kxy],
                        visibilities=vis,
                        bbox_xyxy=bbox,
                    )
                )

        return PoseResult(
            frame_index=frame_index,
            timestamp_ns=timestamp_ns,
            camera_index=camera_index,
            subjects=subjects,
            backend=f"yolov8-pose/{self._model.ckpt_path}",
            inference_ms=inference_ms,
        )

    # ── Helpers ───────────────────────────────────────────────────────────────

    @staticmethod
    def _auto_device() -> str:
        try:
            import torch

            if torch.cuda.is_available():
                return "cuda:0"
            if torch.backends.mps.is_available():
                return "mps"
        except ImportError:
            pass
        return "cpu"
