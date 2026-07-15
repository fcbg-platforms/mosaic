"""
Mouse centroid tracker using OpenCV MOG2 background subtraction.

Pipeline
--------
1. MOG2 background subtraction → binary foreground mask
2. Morphological close (fill holes) + open (remove salt noise)
3. Contour detection, area-filtered to plausible mouse blob sizes
4. Per-blob centroid and bounding box
5. Greedy nearest-neighbour assignment to existing tracks
6. Track lifecycle management (create / update / mark-lost / prune)
7. Optional mm/px scale conversion and velocity computation
"""

from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import cv2
import numpy as np


# ── Track ──────────────────────────────────────────────────────────────────

@dataclass
class Track:
    id: int
    positions: deque = field(default_factory=lambda: deque(maxlen=90))
    timestamps_ns: deque = field(default_factory=lambda: deque(maxlen=90))
    areas: deque = field(default_factory=lambda: deque(maxlen=90))
    last_frame: int = 0
    lost_count: int = 0

    @property
    def position(self) -> Tuple[float, float]:
        return self.positions[-1] if self.positions else (0.0, 0.0)

    @property
    def velocity_px_per_frame(self) -> float:
        if len(self.positions) < 2:
            return 0.0
        p1, p2 = self.positions[-2], self.positions[-1]
        return math.hypot(p2[0] - p1[0], p2[1] - p1[1])

    def velocity_mm_per_s(self, mm_per_px: float, fps: float) -> float:
        return self.velocity_px_per_frame * mm_per_px * fps

    @property
    def mean_area(self) -> float:
        return float(np.mean(self.areas)) if self.areas else 0.0


# ── Detection ──────────────────────────────────────────────────────────────

@dataclass
class Detection:
    cx: float
    cy: float
    area: float
    bbox: Tuple[int, int, int, int]  # x, y, w, h
    contour: np.ndarray


# ── CentroidTracker ────────────────────────────────────────────────────────

class CentroidTracker:
    """
    Parameters
    ----------
    min_area, max_area
        Contour area thresholds (px²).  Tune to your arena / camera height.
        Typical mouse at ~40 cm camera height: 500–6000 px².
    max_distance
        Maximum centroid movement between frames (px) for assignment.
    max_lost
        Frames a track can be missing before deletion.
    learning_rate
        MOG2 learning rate (-1 = automatic).
    close_kernel, open_kernel
        Morphological structuring element sizes (px).
    history
        MOG2 frame history for background model.
    var_threshold
        MOG2 Mahalanobis distance threshold (lower = more sensitive).
    mm_per_px
        Scale factor.  Calibrate from arena dimensions.
        Default 1.0 means outputs are in pixels.
    n_animals
        Expected number of animals.  Limits track creation to prevent
        phantom tracks from lighting artefacts.  0 = unlimited.
    """

    def __init__(
        self,
        min_area: int = 400,
        max_area: int = 7000,
        max_distance: float = 80.0,
        max_lost: int = 20,
        learning_rate: float = -1,
        close_kernel: int = 9,
        open_kernel: int = 5,
        history: int = 500,
        var_threshold: float = 16.0,
        mm_per_px: float = 1.0,
        n_animals: int = 0,
    ) -> None:
        self.min_area     = min_area
        self.max_area     = max_area
        self.max_distance = max_distance
        self.max_lost     = max_lost
        self.learning_rate = learning_rate
        self.close_kernel = close_kernel
        self.open_kernel  = open_kernel
        self.mm_per_px    = mm_per_px
        self.n_animals    = n_animals

        self._fgbg = cv2.createBackgroundSubtractorMOG2(
            history=history,
            varThreshold=var_threshold,
            detectShadows=True,
        )
        self._next_id: int = 0
        self._tracks: Dict[int, Track] = {}
        self._frame_idx: int = 0

        # Pre-build kernels once
        self._close_k = cv2.getStructuringElement(
            cv2.MORPH_ELLIPSE, (self.close_kernel, self.close_kernel))
        self._open_k  = cv2.getStructuringElement(
            cv2.MORPH_ELLIPSE, (self.open_kernel, self.open_kernel))

    # ── Public ──────────────────────────────────────────────────────────────

    def update(
        self,
        frame: np.ndarray,
        timestamp_ns: int = 0,
        fps: float = 30.0,
    ) -> List[Track]:
        """
        Process one frame and return a list of currently active tracks.

        Parameters
        ----------
        frame
            BGR or grayscale frame from OpenCV.
        timestamp_ns
            Wall-clock nanoseconds for velocity computation.
        fps
            Frames per second (used only for velocity in mm/s).
        """
        detections = self._detect(frame)
        self._assign(detections, timestamp_ns, fps)
        self._prune_lost()
        self._frame_idx += 1
        return list(self._tracks.values())

    @property
    def tracks(self) -> Dict[int, Track]:
        return self._tracks

    def reset(self) -> None:
        self._tracks.clear()
        self._next_id = 0
        self._frame_idx = 0
        self._fgbg = cv2.createBackgroundSubtractorMOG2(
            history=500, varThreshold=16.0, detectShadows=True)

    def set_roi(self, mask: np.ndarray) -> None:
        """
        Restrict detection to an ROI.  ``mask`` is a binary uint8 array
        (255 = keep, 0 = ignore) of the same size as the input frame.
        """
        self._roi_mask = mask

    # ── Internal ────────────────────────────────────────────────────────────

    def _detect(self, frame: np.ndarray) -> List[Detection]:
        gray = frame if frame.ndim == 2 else cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

        # Background subtraction; shadow pixels = 127 → threshold to 0
        fg = self._fgbg.apply(gray, learningRate=self.learning_rate)
        _, fg = cv2.threshold(fg, 200, 255, cv2.THRESH_BINARY)

        # Morphological clean-up
        fg = cv2.morphologyEx(fg, cv2.MORPH_CLOSE, self._close_k)
        fg = cv2.morphologyEx(fg, cv2.MORPH_OPEN,  self._open_k)

        # Apply optional ROI mask
        if hasattr(self, "_roi_mask") and self._roi_mask is not None:
            fg = cv2.bitwise_and(fg, self._roi_mask)

        contours, _ = cv2.findContours(fg, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

        detections: List[Detection] = []
        for cnt in contours:
            area = cv2.contourArea(cnt)
            if area < self.min_area or area > self.max_area:
                continue
            M = cv2.moments(cnt)
            if M["m00"] == 0:
                continue
            cx = M["m10"] / M["m00"]
            cy = M["m01"] / M["m00"]
            x, y, w, h = cv2.boundingRect(cnt)
            detections.append(Detection(cx=cx, cy=cy, area=area,
                                        bbox=(x, y, w, h), contour=cnt))

        # Sort largest first — helps with occluded blobs
        detections.sort(key=lambda d: d.area, reverse=True)

        # Honour n_animals cap
        if self.n_animals > 0:
            detections = detections[: self.n_animals]

        return detections

    def _assign(
        self,
        detections: List[Detection],
        timestamp_ns: int,
        fps: float,
    ) -> None:
        if not self._tracks:
            # No existing tracks → create one per detection
            for det in detections:
                self._create_track(det, timestamp_ns)
            return

        active_ids = list(self._tracks.keys())
        track_positions = np.array(
            [self._tracks[tid].position for tid in active_ids], dtype=np.float64)
        det_positions = np.array(
            [(d.cx, d.cy) for d in detections], dtype=np.float64)

        # Build cost matrix (Euclidean distances)
        if det_positions.size == 0:
            for tid in active_ids:
                self._tracks[tid].lost_count += 1
            return

        diffs = track_positions[:, np.newaxis, :] - det_positions[np.newaxis, :, :]
        cost  = np.hypot(diffs[..., 0], diffs[..., 1])  # shape (n_tracks, n_dets)

        matched_tracks: set = set()
        matched_dets:   set = set()

        # Greedy assignment: repeatedly pick the minimum-cost pair
        while True:
            if cost.size == 0:
                break
            min_idx = np.unravel_index(np.argmin(cost), cost.shape)
            ti, di  = int(min_idx[0]), int(min_idx[1])
            if cost[ti, di] > self.max_distance:
                break
            tid = active_ids[ti]
            self._update_track(self._tracks[tid], detections[di], timestamp_ns, fps)
            matched_tracks.add(ti)
            matched_dets.add(di)
            cost[ti, :] = np.inf
            cost[:, di] = np.inf

        # Unmatched tracks — increment lost counter
        for ti, tid in enumerate(active_ids):
            if ti not in matched_tracks:
                self._tracks[tid].lost_count += 1

        # Unmatched detections — create new tracks if under cap
        for di, det in enumerate(detections):
            if di not in matched_dets:
                if self.n_animals == 0 or len(self._tracks) < self.n_animals:
                    self._create_track(det, timestamp_ns)

    def _create_track(self, det: Detection, ts_ns: int) -> Track:
        t = Track(id=self._next_id, last_frame=self._frame_idx)
        t.positions.append((det.cx, det.cy))
        t.timestamps_ns.append(ts_ns)
        t.areas.append(det.area)
        self._tracks[self._next_id] = t
        self._next_id += 1
        return t

    def _update_track(
        self, track: Track, det: Detection, ts_ns: int, fps: float
    ) -> None:
        track.positions.append((det.cx, det.cy))
        track.timestamps_ns.append(ts_ns)
        track.areas.append(det.area)
        track.last_frame = self._frame_idx
        track.lost_count = 0

    def _prune_lost(self) -> None:
        to_delete = [tid for tid, t in self._tracks.items()
                     if t.lost_count > self.max_lost]
        for tid in to_delete:
            del self._tracks[tid]


# ── Annotated frame ─────────────────────────────────────────────────────────

def draw_tracks(
    frame: np.ndarray,
    tracks: List[Track],
    trail_length: int = 30,
    mm_per_px: float = 1.0,
    fps: float = 30.0,
) -> np.ndarray:
    """Overlay tracks, centroids, IDs and velocity on a BGR frame (in-place)."""
    PALETTE = [
        (255, 80,  80),
        (80, 255,  80),
        (80,  80, 255),
        (255, 255,  80),
        (255,  80, 255),
        (80, 255, 255),
        (200, 140,  60),
        (140,  60, 200),
    ]

    out = frame.copy()
    for track in tracks:
        color = PALETTE[track.id % len(PALETTE)]

        # Trail
        pts = list(track.positions)[-trail_length:]
        for i in range(1, len(pts)):
            alpha = i / len(pts)
            c = tuple(int(v * alpha) for v in color)
            cv2.line(out,
                     (int(pts[i-1][0]), int(pts[i-1][1])),
                     (int(pts[i][0]),   int(pts[i][1])),
                     c, 1, cv2.LINE_AA)

        # Current centroid
        if pts:
            cx, cy = int(pts[-1][0]), int(pts[-1][1])
            cv2.circle(out, (cx, cy), 6, color, -1, cv2.LINE_AA)
            vel = track.velocity_mm_per_s(mm_per_px, fps)
            label = f"#{track.id}  {vel:.1f} mm/s"
            cv2.putText(out, label, (cx + 8, cy - 6),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.42, color, 1, cv2.LINE_AA)

    return out
