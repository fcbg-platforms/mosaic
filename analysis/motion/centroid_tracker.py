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

import cv2
import numpy as np

# ── Track ──────────────────────────────────────────────────────────────────


@dataclass
class Track:
    """One tracked animal's recent position/area history.

    Attributes
    ----------
    id : int
        Stable track ID, assigned once at creation.
    positions : collections.deque
        Recent ``(cx, cy)`` centroid history, most recent last, capped to
        the last 90 frames.
    timestamps_ns : collections.deque
        Recent per-frame timestamps (ns), parallel to ``positions``.
    areas : collections.deque
        Recent per-frame contour areas (px²), parallel to ``positions``.
    last_frame : int, default 0
        Internal frame index this track was last updated at.
    lost_count : int, default 0
        Consecutive frames since this track was last matched to a
        detection; pruned once this exceeds
        :attr:`CentroidTracker.max_lost`.
    """

    id: int
    positions: deque = field(default_factory=lambda: deque(maxlen=90))
    timestamps_ns: deque = field(default_factory=lambda: deque(maxlen=90))
    areas: deque = field(default_factory=lambda: deque(maxlen=90))
    last_frame: int = 0
    lost_count: int = 0

    @property
    def position(self) -> tuple[float, float]:
        """tuple of float: Most recent ``(cx, cy)`` centroid, or ``(0.0, 0.0)`` if none yet."""
        return self.positions[-1] if self.positions else (0.0, 0.0)

    @property
    def velocity_px_per_frame(self) -> float:
        """float: Euclidean distance between the last two centroids, px/frame."""
        if len(self.positions) < 2:
            return 0.0
        p1, p2 = self.positions[-2], self.positions[-1]
        return math.hypot(p2[0] - p1[0], p2[1] - p1[1])

    def velocity_mm_per_s(self, mm_per_px: float, fps: float) -> float:
        """Convert :attr:`velocity_px_per_frame` to a real-world speed.

        Parameters
        ----------
        mm_per_px : float
            Manual pixel-to-real-world scale factor.
        fps : float
            Assumed nominal frame rate.

        Returns
        -------
        float
            Speed in mm/s.

        Notes
        -----
        Uses a fixed nominal ``fps`` rather than each sample's real
        elapsed time, unlike the more careful real-Δt derivative in
        :doc:`/math/pose_kinematics`. See :doc:`/math/motion_tracking`
        for the contrast.
        """
        return self.velocity_px_per_frame * mm_per_px * fps

    @property
    def mean_area(self) -> float:
        """float: Mean contour area (px²) over this track's recent history."""
        return float(np.mean(self.areas)) if self.areas else 0.0


# ── Detection ──────────────────────────────────────────────────────────────


@dataclass
class Detection:
    """One per-frame foreground blob, before track assignment.

    Attributes
    ----------
    cx, cy : float
        Blob centroid (image moments), px.
    area : float
        Contour area, px².
    bbox : tuple of int
        Bounding rect, ``(x, y, w, h)`` px.
    contour : numpy.ndarray
        The raw OpenCV contour this detection was built from.
    """

    cx: float
    cy: float
    area: float
    bbox: tuple[int, int, int, int]  # x, y, w, h
    contour: np.ndarray


# ── CentroidTracker ────────────────────────────────────────────────────────


class CentroidTracker:
    """Greedy nearest-neighbour multi-object tracker over MOG2 foreground blobs.

    See the module docstring for the full 7-step pipeline.

    Parameters
    ----------
    min_area, max_area : int
        Contour area thresholds (px²). Tune to your arena / camera
        height. Typical mouse at ~40 cm camera height: 500-6000 px².
    max_distance : float, default 80.0
        Maximum centroid movement between frames (px) for assignment.
        See :doc:`/math/motion_tracking` for the assignment rule.
    max_lost : int, default 20
        Frames a track can be missing before deletion.
    learning_rate : float, default -1
        MOG2 learning rate (``-1`` = automatic).
    close_kernel, open_kernel : int
        Morphological structuring element sizes (px).
    history : int, default 500
        MOG2 frame history for the background model.
    var_threshold : float, default 16.0
        MOG2 Mahalanobis distance threshold (lower = more sensitive).
    mm_per_px : float, default 1.0
        Manual pixel-to-real-world scale factor, calibrated from arena
        dimensions. ``1.0`` means outputs stay in pixels.
    n_animals : int, default 0
        Expected number of animals. Limits track creation to prevent
        phantom tracks from lighting artefacts. ``0`` = unlimited.
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
        self.min_area = min_area
        self.max_area = max_area
        self.max_distance = max_distance
        self.max_lost = max_lost
        self.learning_rate = learning_rate
        self.close_kernel = close_kernel
        self.open_kernel = open_kernel
        self.mm_per_px = mm_per_px
        self.n_animals = n_animals

        self._fgbg = cv2.createBackgroundSubtractorMOG2(
            history=history,
            varThreshold=var_threshold,
            detectShadows=True,
        )
        self._next_id: int = 0
        self._tracks: dict[int, Track] = {}
        self._frame_idx: int = 0

        # Pre-build kernels once
        self._close_k = cv2.getStructuringElement(
            cv2.MORPH_ELLIPSE, (self.close_kernel, self.close_kernel)
        )
        self._open_k = cv2.getStructuringElement(
            cv2.MORPH_ELLIPSE, (self.open_kernel, self.open_kernel)
        )

    # ── Public ──────────────────────────────────────────────────────────────

    def update(
        self,
        frame: np.ndarray,
        timestamp_ns: int = 0,
        fps: float = 30.0,
    ) -> list[Track]:
        """Process one frame and return the currently active tracks.

        Parameters
        ----------
        frame : numpy.ndarray
            BGR or grayscale frame from OpenCV.
        timestamp_ns : int, default 0
            Wall-clock nanoseconds for velocity computation.
        fps : float, default 30.0
            Frames per second (used only for velocity in mm/s).

        Returns
        -------
        list of Track
            Every currently active (not-yet-pruned) track, including
            ones not matched this frame.
        """
        detections = self._detect(frame)
        self._assign(detections, timestamp_ns, fps)
        self._prune_lost()
        self._frame_idx += 1
        return list(self._tracks.values())

    @property
    def tracks(self) -> dict[int, Track]:
        return self._tracks

    def reset(self) -> None:
        """Clear all tracks and rebuild the background model from scratch."""
        self._tracks.clear()
        self._next_id = 0
        self._frame_idx = 0
        self._fgbg = cv2.createBackgroundSubtractorMOG2(
            history=500, varThreshold=16.0, detectShadows=True
        )

    def set_roi(self, mask: np.ndarray) -> None:
        """Restrict detection to a region of interest.

        Parameters
        ----------
        mask : numpy.ndarray
            Binary ``uint8`` array (255 = keep, 0 = ignore), the same
            size as the input frame.
        """
        self._roi_mask = mask

    # ── Internal ────────────────────────────────────────────────────────────

    def _detect(self, frame: np.ndarray) -> list[Detection]:
        gray = frame if frame.ndim == 2 else cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

        # Background subtraction; shadow pixels = 127 → threshold to 0
        fg = self._fgbg.apply(gray, learningRate=self.learning_rate)
        _, fg = cv2.threshold(fg, 200, 255, cv2.THRESH_BINARY)

        # Morphological clean-up
        fg = cv2.morphologyEx(fg, cv2.MORPH_CLOSE, self._close_k)
        fg = cv2.morphologyEx(fg, cv2.MORPH_OPEN, self._open_k)

        # Apply optional ROI mask
        if hasattr(self, "_roi_mask") and self._roi_mask is not None:
            fg = cv2.bitwise_and(fg, self._roi_mask)

        contours, _ = cv2.findContours(fg, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

        detections: list[Detection] = []
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
            detections.append(Detection(cx=cx, cy=cy, area=area, bbox=(x, y, w, h), contour=cnt))

        # Sort largest first — helps with occluded blobs
        detections.sort(key=lambda d: d.area, reverse=True)

        # Honour n_animals cap
        if self.n_animals > 0:
            detections = detections[: self.n_animals]

        return detections

    def _assign(
        self,
        detections: list[Detection],
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
            [self._tracks[tid].position for tid in active_ids], dtype=np.float64
        )
        det_positions = np.array([(d.cx, d.cy) for d in detections], dtype=np.float64)

        # Build cost matrix (Euclidean distances)
        if det_positions.size == 0:
            for tid in active_ids:
                self._tracks[tid].lost_count += 1
            return

        diffs = track_positions[:, np.newaxis, :] - det_positions[np.newaxis, :, :]
        cost = np.hypot(diffs[..., 0], diffs[..., 1])  # shape (n_tracks, n_dets)

        matched_tracks: set = set()
        matched_dets: set = set()

        # Greedy assignment: repeatedly pick the minimum-cost pair
        while True:
            if cost.size == 0:
                break
            min_idx = np.unravel_index(np.argmin(cost), cost.shape)
            ti, di = int(min_idx[0]), int(min_idx[1])
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

    def _update_track(self, track: Track, det: Detection, ts_ns: int, fps: float) -> None:
        track.positions.append((det.cx, det.cy))
        track.timestamps_ns.append(ts_ns)
        track.areas.append(det.area)
        track.last_frame = self._frame_idx
        track.lost_count = 0

    def _prune_lost(self) -> None:
        to_delete = [tid for tid, t in self._tracks.items() if t.lost_count > self.max_lost]
        for tid in to_delete:
            del self._tracks[tid]


# ── Annotated frame ─────────────────────────────────────────────────────────


def draw_tracks(
    frame: np.ndarray,
    tracks: list[Track],
    trail_length: int = 30,
    mm_per_px: float = 1.0,
    fps: float = 30.0,
) -> np.ndarray:
    """Overlay tracks, centroids, IDs, and velocity on a BGR frame.

    Parameters
    ----------
    frame : numpy.ndarray
        BGR frame; not modified — a copy is drawn on and returned.
    tracks : list of Track
        Tracks to draw, e.g. from :meth:`CentroidTracker.update`.
    trail_length : int, default 30
        Number of recent positions to draw as a fading trail per track.
    mm_per_px : float, default 1.0
        Forwarded to :meth:`Track.velocity_mm_per_s` for the on-frame
        velocity label.
    fps : float, default 30.0
        Forwarded to :meth:`Track.velocity_mm_per_s` for the on-frame
        velocity label.

    Returns
    -------
    numpy.ndarray
        A new BGR frame (copy of ``frame``) with tracks overlaid.
    """
    PALETTE = [
        (255, 80, 80),
        (80, 255, 80),
        (80, 80, 255),
        (255, 255, 80),
        (255, 80, 255),
        (80, 255, 255),
        (200, 140, 60),
        (140, 60, 200),
    ]

    out = frame.copy()
    for track in tracks:
        color = PALETTE[track.id % len(PALETTE)]

        # Trail
        pts = list(track.positions)[-trail_length:]
        for i in range(1, len(pts)):
            alpha = i / len(pts)
            c = tuple(int(v * alpha) for v in color)
            cv2.line(
                out,
                (int(pts[i - 1][0]), int(pts[i - 1][1])),
                (int(pts[i][0]), int(pts[i][1])),
                c,
                1,
                cv2.LINE_AA,
            )

        # Current centroid
        if pts:
            cx, cy = int(pts[-1][0]), int(pts[-1][1])
            cv2.circle(out, (cx, cy), 6, color, -1, cv2.LINE_AA)
            vel = track.velocity_mm_per_s(mm_per_px, fps)
            label = f"#{track.id}  {vel:.1f} mm/s"
            cv2.putText(
                out, label, (cx + 8, cy - 6), cv2.FONT_HERSHEY_SIMPLEX, 0.42, color, 1, cv2.LINE_AA
            )

    return out
