"""
Cross-frame 3D track identity for the 3D Pose Reconstruction plugin
(analysis/run_pose3d.py). Greedy nearest-3D-centroid assignment between
consecutive analysed ticks — the same greedy-nearest-neighbor idiom
analysis/motion/centroid_tracker.py's CentroidTracker._assign() already
uses for 2D pixel centroids, deliberately reimplemented here for 3D
room-mm centroids rather than shared/generalized, matching this codebase's
established per-plugin-owns-its-math precedent (e.g. item 18's gaze/pose
iris-heuristic duplication).
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass
class TrackedPerson3D:
    track_id: int
    last_tick: int
    last_centroid_room: np.ndarray


class PersonTracker3D:
    def __init__(self, max_gap_ticks: int = 5, max_jump_mm: float = 400.0) -> None:
        self.max_gap_ticks = max_gap_ticks
        self.max_jump_mm = max_jump_mm
        self._tracks: dict = {}
        self._next_id = 0

    def update(self, tick: int, cluster_centroids) -> list:
        """Returns one track_id per input centroid, same order. Existing
        tracks within max_jump_mm are matched greedily (nearest pair
        first); unmatched existing tracks age by (tick - last_tick) and are
        dropped once that exceeds max_gap_ticks; unmatched new centroids
        get a fresh monotonically-increasing track_id."""
        if not cluster_centroids:
            self._age_all(tick)
            return []

        active_ids = list(self._tracks.keys())
        result_ids = [-1] * len(cluster_centroids)
        matched_cents: set = set()

        if active_ids:
            track_pos = np.array([self._tracks[tid].last_centroid_room for tid in active_ids])
            cent_pos = np.array(cluster_centroids)
            diffs = track_pos[:, np.newaxis, :] - cent_pos[np.newaxis, :, :]
            cost = np.linalg.norm(diffs, axis=2)  # (n_tracks, n_centroids)

            while True:
                if cost.size == 0:
                    break
                min_idx = np.unravel_index(np.argmin(cost), cost.shape)
                ti, ci = int(min_idx[0]), int(min_idx[1])
                if cost[ti, ci] > self.max_jump_mm:
                    break
                tid = active_ids[ti]
                self._tracks[tid].last_tick = tick
                self._tracks[tid].last_centroid_room = cluster_centroids[ci]
                result_ids[ci] = tid
                matched_cents.add(ci)
                cost[ti, :] = np.inf
                cost[:, ci] = np.inf

        for ci, centroid in enumerate(cluster_centroids):
            if ci not in matched_cents:
                new_id = self._next_id
                self._next_id += 1
                self._tracks[new_id] = TrackedPerson3D(
                    track_id=new_id, last_tick=tick, last_centroid_room=np.asarray(centroid)
                )
                result_ids[ci] = new_id

        self._age_all(tick)
        return result_ids

    def _age_all(self, tick: int) -> None:
        stale = [tid for tid, t in self._tracks.items() if tick - t.last_tick > self.max_gap_ticks]
        for tid in stale:
            del self._tracks[tid]
