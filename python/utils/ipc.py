"""Helpers for the C++ ↔ Python IPC protocol used by frame_server.py."""

from __future__ import annotations

import struct
from typing import BinaryIO

import numpy as np

_HEADER_FMT = "<IIII"  # cam_idx, width, height, reserved
_HEADER_SIZE = struct.calcsize(_HEADER_FMT)


def write_frame(stream: BinaryIO, cam_idx: int, frame_bgr: np.ndarray) -> None:
    """Write one BGR frame to *stream* in the PoseWorker wire format."""
    h, w = frame_bgr.shape[:2]
    header = struct.pack(_HEADER_FMT, cam_idx, w, h, 0)
    stream.write(header)
    stream.write(frame_bgr.tobytes())
    stream.flush()


def read_frame(stream: BinaryIO) -> tuple[int, np.ndarray] | tuple[None, None]:
    """Read one frame from *stream*. Returns (cam_idx, BGR ndarray) or (None, None) on EOF."""
    header = stream.read(_HEADER_SIZE)
    if len(header) < _HEADER_SIZE:
        return None, None
    cam_idx, width, height, _ = struct.unpack(_HEADER_FMT, header)
    n_bytes = width * height * 3
    payload = stream.read(n_bytes)
    if len(payload) < n_bytes:
        return None, None
    frame = np.frombuffer(payload, dtype=np.uint8).reshape((height, width, 3))
    return cam_idx, frame
