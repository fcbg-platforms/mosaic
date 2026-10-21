"""Writing an annotated video, and putting it in place of the previous one.

:func:`open_writer` picks the codec. Every annotated video is written to a
``.partial.mp4`` file first and only renamed onto its real name once
complete. On Windows that rename fails while another program has the old
file open: MOSAIC's own player showing it, or a video player. This waits
briefly for the file to come free, then says which file is in the way
instead of a bare ``PermissionError``.
"""

from __future__ import annotations

import ctypes
import sys
import time
from functools import cache
from pathlib import Path

#: The library OpenCV's FFmpeg plugin needs for H.264 (avc1) on Windows. It is
#: not shipped with opencv-python; without it an avc1 writer on that plugin
#: fails with "Unable to create encoder" and leaves the plugin in a state that
#: has crashed Python on exit (0xC0000005 in opencv_videoio_ffmpeg) after the
#: work was done.
OPENH264_DLL = "openh264-2.5.0-win64.dll"


@cache
def has_openh264() -> bool:
    """Whether OpenCV's FFmpeg plugin can write H.264 here (Windows only)."""
    try:
        ctypes.WinDLL(OPENH264_DLL)
    except OSError:
        return False
    return True


def writer_attempts() -> list:
    """``(fourcc, backend)`` pairs to try, best first.

    H.264 (avc1) plays most reliably in Qt and Windows Media Foundation, and
    keeps the picture: re-encoding a recording as MPEG-4 (mp4v) instead loses
    about 5 dB of PSNR. On Windows it comes from the FFmpeg plugin only with
    OpenH264, otherwise from Media Foundation's own encoder (larger files,
    about 24 Mbit/s at 1080p); MPEG-4 is the last resort every build has.
    """
    import cv2

    if sys.platform != "win32":
        return [("avc1", cv2.CAP_ANY), ("mp4v", cv2.CAP_ANY)]
    first = [("avc1", cv2.CAP_FFMPEG)] if has_openh264() else []
    return first + [("avc1", cv2.CAP_MSMF), ("mp4v", cv2.CAP_FFMPEG)]


def open_writer(path: Path, fps: float, size: tuple[int, int]):
    """An mp4 writer from the first of :func:`writer_attempts` that opens;
    ``None`` when none does."""
    import cv2

    for code, backend in writer_attempts():
        w = cv2.VideoWriter(str(path), backend, cv2.VideoWriter_fourcc(*code), fps, size)
        if w.isOpened():
            return w
        w.release()
    return None


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
