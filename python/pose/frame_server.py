"""
Real-time pose IPC server.

Reads raw BGR frames from stdin (sent by the C++ PoseWorker), runs MediaPipe
Pose on each frame, and writes JSON keypoints to stdout.

stdin protocol (per frame):
    bytes 0-3   : camera index (uint32 LE)
    bytes 4-7   : frame width  (uint32 LE)
    bytes 8-11  : frame height (uint32 LE)
    bytes 12-15 : reserved / padding (uint32 LE, always 0)
    bytes 16+   : raw BGR8 pixel data (width * height * 3 bytes)

stdout protocol (per frame, newline-terminated JSON):
    {"camera": <int>, "keypoints": [{"x":.., "y":.., "z":..,
                                      "visibility":.., "name":..}, ...],
     "timestamp": <float epoch seconds>}

Empty keypoints list means no person was detected.
"""
from __future__ import annotations

import json
import os
import struct
import sys
import time

# When launched as a script, Python adds this file's directory (python/pose/)
# to sys.path, making "from pose.estimator import …" fail because the pose
# package is a sibling of the script, not a child.  Add the parent so that
# "import pose" resolves to python/pose/.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np

from pose.estimator import PoseEstimator
from pose.gaze_estimator import GazeEstimator


def read_frame() -> tuple[int, np.ndarray] | tuple[None, None]:
    """Read one frame header + payload from stdin.

    Returns (camera_index, BGR ndarray) or (None, None) on EOF / error.
    """
    header = sys.stdin.buffer.read(16)
    if len(header) < 16:
        return None, None

    cam_idx, width, height, _ = struct.unpack_from("<IIII", header)
    n_bytes = width * height * 3
    payload = sys.stdin.buffer.read(n_bytes)
    if len(payload) < n_bytes:
        return None, None

    frame = np.frombuffer(payload, dtype=np.uint8).reshape((height, width, 3))
    return cam_idx, frame


def main() -> None:
    estimator      = PoseEstimator(model_complexity=1)
    gaze_estimator = GazeEstimator()
    try:
        while True:
            cam_idx, frame = read_frame()
            if frame is None:
                break

            keypoints = estimator.estimate(frame)
            gaze      = gaze_estimator.estimate(frame)
            result = {
                "camera":    cam_idx,
                "keypoints": [kp.to_dict() for kp in keypoints] if keypoints else [],
                "gaze":      gaze.to_dict() if gaze else None,
                "timestamp": time.time(),
            }
            sys.stdout.write(json.dumps(result) + "\n")
            sys.stdout.flush()
    finally:
        estimator.close()
        gaze_estimator.close()


if __name__ == "__main__":
    main()
