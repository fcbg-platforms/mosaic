"""Transcript segment times from word timestamps, and publishing a video
over a file that is still open."""

from __future__ import annotations

import sys
from pathlib import Path
from types import SimpleNamespace

import pytest
import video_publish
from diarize import pipeline
from video_publish import open_writer, publish_video


def _word(start, end):
    return SimpleNamespace(start=start, end=end)


def test_segment_runs_from_first_to_last_word():
    seg = SimpleNamespace(start=7.0, end=10.0, words=[_word(7.56, 7.8), _word(8.4, 8.8)])
    assert pipeline.segment_bounds(seg) == {"start": 7.56, "end": 8.8}


def test_segment_without_words_keeps_its_own_times():
    assert pipeline.segment_bounds(SimpleNamespace(start=1.0, end=2.0, words=None)) == {
        "start": 1.0,
        "end": 2.0,
    }
    # Zero-length words carry no timing.
    seg = SimpleNamespace(start=1.0, end=2.0, words=[_word(1.5, 1.5)])
    assert pipeline.segment_bounds(seg) == {"start": 1.0, "end": 2.0}


def test_transcribe_audio_asks_for_word_timestamps():
    calls = {}

    class Model:
        def transcribe(self, path, **kwargs):
            calls.update(kwargs)
            seg = SimpleNamespace(start=0.0, end=5.0, text=" Hi. ", words=[_word(0.4, 1.2)])
            return iter([seg]), SimpleNamespace(language="en")

    segments, lang = pipeline.transcribe_audio(Model(), Path("a.wav"), "en")
    assert calls["word_timestamps"] is True
    assert segments == [{"start": 0.4, "end": 1.2, "text": "Hi."}]
    assert lang == "en"


def test_publish_replaces_the_old_video(tmp_path):
    partial, out = tmp_path / "v.partial.mp4", tmp_path / "v.mp4"
    out.write_bytes(b"old")
    partial.write_bytes(b"new")
    publish_video(partial, out)
    assert out.read_bytes() == b"new"
    assert not partial.exists()


def test_publish_names_the_file_still_open(tmp_path, monkeypatch):
    partial, out = tmp_path / "v.partial.mp4", tmp_path / "v.mp4"
    partial.write_bytes(b"new")

    def locked(self, target):
        raise PermissionError(5, "Access is denied")

    monkeypatch.setattr(Path, "replace", locked)
    with pytest.raises(PermissionError, match=r"v\.mp4 is open in another program"):
        publish_video(partial, out, wait_s=0.3)
    assert not partial.exists()


def test_publish_waits_for_the_file_to_come_free(tmp_path, monkeypatch):
    partial, out = tmp_path / "v.partial.mp4", tmp_path / "v.mp4"
    partial.write_bytes(b"new")
    real = Path.replace
    tries = []

    def busy_once(self, target):
        tries.append(1)
        if len(tries) == 1:
            raise PermissionError(5, "Access is denied")
        return real(self, target)

    monkeypatch.setattr(Path, "replace", busy_once)
    publish_video(partial, out, wait_s=5.0)
    assert out.read_bytes() == b"new"
    assert len(tries) == 2


@pytest.mark.skipif(sys.platform != "win32", reason="the codec choice is Windows-only")
def test_writer_skips_the_ffmpeg_h264_attempt_without_openh264(tmp_path, monkeypatch):
    # Without OpenH264 the FFmpeg plugin's avc1 writer fails noisily and has
    # left the plugin crashing Python on exit: go to Media Foundation's H.264
    # first (as OpenCV's own fallback did), MPEG-4 only if that fails too.
    import cv2
    import numpy as np

    monkeypatch.setattr(video_publish, "has_openh264", lambda: False)
    assert video_publish.writer_attempts() == [
        ("avc1", cv2.CAP_MSMF),
        ("mp4v", cv2.CAP_FFMPEG),
    ]
    monkeypatch.setattr(video_publish, "has_openh264", lambda: True)
    assert video_publish.writer_attempts()[0] == ("avc1", cv2.CAP_FFMPEG)

    monkeypatch.setattr(video_publish, "has_openh264", lambda: False)
    out = tmp_path / "v.mp4"
    w = open_writer(out, 10.0, (64, 48))
    assert w is not None
    w.write(np.zeros((48, 64, 3), np.uint8))
    w.release()
    assert out.stat().st_size > 0
