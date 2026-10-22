"""Which cameras an analysis runs on.

The Analysis tab's camera picker passes ``--camera`` with the chosen camera
indices (0-based, as in ``video_N.mp4``); without it every camera is analysed.
Each camera's results are its own files, so analysing some cameras leaves the
others' results as they were.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


def add_camera_arg(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--camera",
        type=int,
        nargs="+",
        default=None,
        metavar="N",
        help="Only these camera indices, as in video_N.mp4 (default: every camera)",
    )


def pick_cameras(videos: list, cameras: list | None) -> list:
    """``videos`` (paths) of the given camera indices; all of them when
    ``cameras`` is empty or ``None``. A video not named ``video_N`` belongs
    to no camera, so it is left out of a pick."""
    if not cameras:
        return list(videos)
    want = set(cameras)
    out = []
    for v in videos:
        m = re.fullmatch(r"video_(\d+)", Path(v).stem)
        if m and int(m.group(1)) in want:
            out.append(v)
    return out
