"""
Tests for rppg/hrv.py: beat-to-beat timing and HRV on a synthetic face
colour signal built from a known beat sequence.
"""

import math
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from rppg import hrv

# Pulse direction in RGB of blood volume changes in skin (normalised).
PULSE_RGB = np.array([0.33, 0.77, 0.53])


def _beat_times(duration_s, rng, mean_s=0.8, rsa_s=0.04, rsa_hz=0.25, jitter_s=0.015):
    """Beat times with breathing-linked variation (RSA) and random variation."""
    t, out = 1.0, []
    while t < duration_s:
        out.append(t)
        t += mean_s + rsa_s * math.sin(2 * math.pi * rsa_hz * t) + rng.normal(0, jitter_s)
    return np.asarray(out)


def _face_colour(beats, fs, duration_s, rng, noise=0.0004, frame_jitter_s=0.002):
    """Per-frame mean skin colour with a pulse at the given beats, slow
    illumination drift and camera noise; frame times jitter slightly."""
    t = np.arange(0, duration_s, 1.0 / fs) + rng.normal(0, frame_jitter_s, int(duration_s * fs))
    t = np.sort(t)
    wave = np.zeros(t.size)
    for b in beats:
        dt = t - b
        wave += np.exp(-0.5 * (dt / 0.08) ** 2) + 0.3 * np.exp(-0.5 * ((dt - 0.3) / 0.06) ** 2)
    light = 1.0 + 0.05 * np.sin(2 * np.pi * 0.02 * t)  # slow light change
    base = np.array([150.0, 110.0, 90.0])
    rgb = light[:, None] * base * (1.0 + 0.004 * wave[:, None] * PULSE_RGB)
    return t, rgb + rng.normal(0, noise * 150.0, rgb.shape)


def _truth(beats):
    ibi = np.diff(beats)
    d = np.diff(ibi)
    return {
        "mean_hr": 60.0 / ibi.mean(),
        "rmssd_ms": 1000.0 * math.sqrt(np.mean(d**2)),
        "sdnn_ms": 1000.0 * ibi.std(ddof=1),
    }


def _split_face(beats, fs, duration_s, seed):
    """Two face halves with independent noise, and the whole face (their mean)."""
    t, left = _face_colour(
        beats, fs, duration_s, np.random.default_rng(seed + 10), noise=0.00057, frame_jitter_s=0.0
    )
    _, right = _face_colour(
        beats, fs, duration_s, np.random.default_rng(seed + 20), noise=0.00057, frame_jitter_s=0.0
    )
    return t, (left + right) / 2.0, left, right


def test_beats_and_hrv_are_recovered_at_50_fps():
    rng = np.random.default_rng(0)
    beats = _beat_times(180.0, rng)
    t, whole, left, right = _split_face(beats, 50.0, 182.0, 0)
    r = hrv.analyse(t, whole, 50.0, halves=(left, right))
    found = r["beats_s"]
    # Around the fixed upstroke-to-peak offset, beats are found within tens
    # of milliseconds (the pulse is noisy, as it is on a real face).
    nearest = np.array([found[np.argmin(np.abs(found - b))] - b for b in beats[2:-2]])
    err = np.abs(nearest - np.median(nearest))
    assert np.median(err) < 0.02 and np.mean(err < 0.06) > 0.95
    assert r["hrv"] is not None, r["hrv_withheld"]
    truth = _truth(beats)
    h = r["hrv"]
    assert h["mean_hr_bpm"] == pytest.approx(truth["mean_hr"], abs=1.0)
    # Timing noise inflates the raw values; the split-face correction takes
    # most of that back out.
    assert h["rmssd_ms"] > 1.3 * truth["rmssd_ms"]
    assert h["rmssd_corrected_ms"] == pytest.approx(truth["rmssd_ms"], rel=0.25)
    assert h["sdnn_corrected_ms"] == pytest.approx(truth["sdnn_ms"], rel=0.2)
    assert 10.0 < h["timing_jitter_ms"] < 30.0 and r["jitter_pairs"] > 100
    # Breathing at 0.25 Hz shows as the HF peak.
    assert h["hf_peak_hz"] == pytest.approx(0.25, abs=0.03)
    assert r["artifact_share"] < 0.1
    assert len(r["windows"]) > 10 and r["windows"][0]["hr_bpm"] is not None


def test_jitter_from_two_beat_series():
    rng = np.random.default_rng(5)
    beats = np.arange(0.0, 100.0, 0.8)
    a = beats + rng.normal(0, 0.014, beats.size)  # each half: 14 ms of noise
    b = beats + 0.03 + rng.normal(0, 0.014, beats.size)  # plus a constant offset
    j, pairs = hrv.timing_jitter(a, np.ones(a.size), b, np.ones(b.size))
    # The whole face (both halves averaged) has 14 / sqrt(2) ms.
    assert j * 1000 == pytest.approx(14 / math.sqrt(2), rel=0.2) and pairs == beats.size
    assert hrv.timing_jitter(a[:10], np.ones(10), b[:10], np.ones(10))[0] is None
    assert hrv.corrected(50.0, 10.0, 6.0) == pytest.approx(math.sqrt(2500 - 600), abs=0.1)
    assert hrv.corrected(10.0, 10.0, 6.0) == 0.0


def test_hrv_is_withheld_at_a_room_frame_rate():
    rng = np.random.default_rng(1)
    beats = _beat_times(120.0, rng)
    t, rgb = _face_colour(beats, 13.0, 122.0, rng)
    r = hrv.analyse(t, rgb, 13.0)
    assert r["hrv"] is None
    assert any("frame rate" in reason for reason in r["hrv_withheld"])


def test_a_long_gap_splits_segments_and_short_recordings_withhold_hrv():
    rng = np.random.default_rng(2)
    beats = _beat_times(50.0, rng)
    t, rgb = _face_colour(beats, 50.0, 52.0, rng)
    keep = (t < 20.0) | (t > 23.0)  # face lost for 3 s
    segs = hrv.resample_uniform(t[keep], rgb[keep], 50.0)
    assert len(segs) == 2 and segs[0][0][-1] < 20.0 and segs[1][0][0] > 23.0
    r = hrv.analyse(t[keep], rgb[keep], 50.0)
    assert r["hrv"] is None and any("clean beats" in x for x in r["hrv_withheld"])
    # No interval spans the gap.
    spans = (r["ibi"]["t_s"] > 23.0) & (r["ibi"]["t_s"] - r["ibi"]["ibi_s"] < 20.0)
    assert not np.any(r["ibi"]["nn"][spans])


def test_a_missed_beat_and_a_poor_beat_are_not_nn():
    beats = np.arange(0.0, 40.0, 0.8)
    q = np.ones(beats.size)
    beats = np.delete(beats, 20)  # one beat missed: a 1.6 s interval
    q = np.delete(q, 20)
    q[30] = 0.2  # a misshapen beat
    iv = hrv.clean_intervals(beats, q, np.zeros(beats.size, int))
    assert not iv["nn"][19]  # the 1.6 s interval
    assert not iv["nn"][29] and not iv["nn"][30]  # both intervals touching the poor beat
    assert iv["nn"].sum() == iv["nn"].size - 3


def test_time_domain_measures():
    ibi = np.array([0.80, 0.82, 0.78, 0.85, 0.80])
    nn = np.array([True, True, True, True, True])
    td = hrv.hrv_time_domain(ibi, nn)
    d = np.diff(ibi)
    assert td["rmssd_ms"] == pytest.approx(1000 * math.sqrt(np.mean(d**2)), abs=0.1)
    assert td["pnn50_pct"] == pytest.approx(100 * np.mean(np.abs(d) > 0.05), abs=0.1)
    assert td["mean_hr_bpm"] == pytest.approx(60 / ibi.mean(), abs=0.1)
    # A non-NN interval breaks the successive differences around it.
    nn[2] = False
    assert hrv._consecutive_nn_diffs(ibi, nn).size == 2


def test_pos_overlap_add_cancels_a_common_light_change():
    fs = 50.0
    t = np.arange(0, 20, 1 / fs)
    pulse = np.sin(2 * np.pi * 1.2 * t)
    light = 1.0 + 0.2 * np.sin(2 * np.pi * 0.1 * t)
    rgb = light[:, None] * np.array([150.0, 110.0, 90.0]) * (1 + 0.003 * pulse[:, None] * PULSE_RGB)
    h = hrv.pos_overlap_add(rgb, fs)
    hz = hrv.dominant_hz(hrv.bandpass(h, fs, 0.7, 3.0), fs)
    assert hz == pytest.approx(1.2, abs=0.05)
    r = np.corrcoef(h[100:-100], pulse[100:-100])[0, 1]
    assert abs(r) > 0.9


def test_rmssd_noise_floor():
    assert hrv.rmssd_noise_floor_ms(10.0) == pytest.approx(24.49, abs=0.01)


def test_runner_writes_beats_hrv_and_state_split(tmp_path):
    import json

    import run_rppg

    rng = np.random.default_rng(3)
    beats = _beat_times(150.0, rng) + 1000.0  # app-launch elapsed seconds
    t, whole, left, right = _split_face(beats - 1000.0, 50.0, 152.0, 3)
    t = t + 1000.0
    ts_ms = list(t * 1000.0)
    session = tmp_path
    (session / "conversation").mkdir()
    spurts = [
        {
            "speaker": "subject",
            "start_ms": 1_000_000 + 10_000,
            "end_ms": 1_000_000 + 70_000,
            "kind": "turn",
        },
        {
            "speaker": "other",
            "start_ms": 1_000_000 + 80_000,
            "end_ms": 1_000_000 + 140_000,
            "kind": "turn",
        },
    ]
    (session / "conversation" / "video_2.conversation.json").write_text(
        json.dumps({"spurts": spurts})
    )
    halves = [(tuple(a), tuple(b)) for a, b in zip(left, right, strict=True)]
    doc = run_rppg._beats_and_hrv(
        Path("video_2.mp4"), ts_ms, list(map(tuple, whole)), halves, ts_ms, 50.0, session / "rppg"
    )
    assert doc["frame_rate"] == pytest.approx(50.0, abs=0.1)
    assert doc["hrv"] is not None and doc["hrv"]["rmssd_corrected_ms"] is not None
    assert doc["beats"][0]["t_ms"] > 1_000_000  # on the video's clock
    assert all(set(x) == {"t_ms", "ibi_ms", "nn"} for x in doc["intervals"])
    assert doc["by_state"]["speaking"]["beats"] > 50 and doc["by_state"]["listening"]["mean_hr_bpm"]
    json.dumps(doc)  # every value is plain JSON
    assert run_rppg._beats_and_hrv(Path("v.mp4"), [], [], [], [], 50.0, None)["hrv"] is None


def test_low_frame_rate_withholds_without_crashing():
    rng = np.random.default_rng(4)
    beats = _beat_times(60.0, rng)
    t, rgb = _face_colour(beats, 5.0, 62.0, rng)
    r = hrv.analyse(t, rgb, 5.0)  # 5 fps: the 3 Hz band edge is above Nyquist
    assert r["hrv"] is None and r["windows"] == [] and r["beats_s"].size == 0


def test_sliding_windows_need_enough_beats():
    t = np.arange(0.8, 200.0, 0.8)
    ibi = np.full(t.size, 0.8)
    nn = np.ones(t.size, bool)
    nn[(t > 60) & (t < 120)] = False  # a minute of artifacts
    w = hrv.windowed_hrv(t, ibi, nn)
    assert any(x["rmssd_ms"] is None for x in w)
    assert all(x["rmssd_ms"] is None or x["nn_count"] >= hrv.MIN_WINDOW_NN for x in w)


def test_split_means_clips_a_roi_past_the_frame_edge():
    from rppg.roi import _split_means

    class P:
        def __init__(self, x, y):
            self.x, self.y = x, y

    h, w = 100, 120
    frame = np.full((h, w, 3), 128, np.uint8)
    mask = np.zeros((h, w), np.uint8)
    mask[40:, :30] = 255
    lm = {1: P(0.05, 0.3), 200: P(0.05, 0.9)}  # midline near the left edge
    # The box starts left of the frame and runs past its bottom.
    halves = _split_means(frame, mask, (-20, 40, 60, 80), lm, w, h)
    assert halves is not None and len(halves) == 2
    assert _split_means(frame, mask, (-50, -50, 10, 10), lm, w, h) is None
