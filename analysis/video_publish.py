"""Putting a finished annotated video in place of the previous one.

Every annotated video is written to a ``.partial.mp4`` file first and only
renamed onto its real name once complete. On Windows that rename fails while
another program has the old file open: MOSAIC's own player showing it, or a
video player. This waits briefly for the file to come free, then says which
file is in the way instead of a bare ``PermissionError``.
"""

from __future__ import annotations

import time
from pathlib import Path


def publish_video(partial: Path, out_path: Path, wait_s: float = 5.0) -> None:
    """Rename `partial` onto `out_path`, retrying while `out_path` is open.

    Raises PermissionError naming the file when it stays open; `partial` is
    removed then, so no half-published file is left behind.
    """
    deadline = time.monotonic() + wait_s
    while True:
        try:
            partial.replace(out_path)
            return
        except PermissionError:
            if time.monotonic() >= deadline:
                partial.unlink(missing_ok=True)
                raise PermissionError(
                    f"{out_path.name} is open in another program (a video player, or "
                    "MOSAIC's player showing it), so it cannot be replaced. Close it and "
                    "run again."
                ) from None
            time.sleep(0.25)
