"""Master-tick-grid alignment for MOSAIC's Frame Sync Repair analysis
plugin — equalizes every camera's frame count in a recorded session by
filling small per-camera frame-count mismatches (GVSP packet loss/trigger
misses) with duplicate-nearest-frame filling."""

from .alignment import (
    AlignmentResult,
    CameraFrames,
    build_tick_grid,
    compute_master_fps,
    compute_window,
)

__all__ = [
    "CameraFrames",
    "AlignmentResult",
    "compute_master_fps",
    "compute_window",
    "build_tick_grid",
]
