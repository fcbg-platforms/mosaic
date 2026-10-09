"""
Tests for face_dynamics/metrics.py: blinks, expression events, expressivity
and head gestures, on synthetic signals with known ground truth.
"""

import math
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from face_dynamics.metrics import (
    EXPRESSIVE,
    angular_speed,
    detect_blinks,
    detect_expression_events,
    detect_head_gestures,
    expressivity,
    eye_aspect_ratio,
    hysteresis_spans,
    openness,
    perclos,
    smooth,
    summarise,
)

FPS = 50.0


def _t(seconds):
    return np.arange(int(seconds * FPS)) / FPS


def _ear_with_blinks(t, blinks, open_ear=0.30, closed_ear=0.03):
    """EAR trace with blinks at (start_s, duration_s), each a smooth dip."""
    ear = np.full(len(t), open_ear)
    for start, dur in blinks:
        inside = (t >= start) & (t < start + dur)
        phase = (t[inside] - start) / dur
        ear[inside] = open_ear - (open_ear - closed_ear) * np.sin(np.pi * phase)
    return ear


# ── Blinks ───────────────────────────────────────────────────────────────────


def test_eye_aspect_ratio_of_an_open_and_a_closed_eye():
    open_eye = [[0, 0], [10, -3], [20, -3], [30, 0], [20, 3], [10, 3]]
    closed = [[0, 0], [10, -0.3], [20, -0.3], [30, 0], [20, 0.3], [10, 0.3]]
    assert eye_aspect_ratio(open_eye) == pytest.approx(0.2)
    assert eye_aspect_ratio(closed) == pytest.approx(0.02)


def test_blinks_are_found_with_their_durations():
    t = _t(20)
    truth = [(2.0, 0.20), (6.0, 0.12), (11.0, 0.30), (15.5, 0.25)]
    ear = _ear_with_blinks(t, truth)
    o = openness(ear, t)
    events = detect_blinks(o, o, t)
    blinks = [e for e in events if e.kind == "blink"]
    assert len(blinks) == 4
    for e, (start, dur) in zip(blinks, truth, strict=True):
        assert abs(e.start_s - start) < dur  # starts within its own duration
        # From below 0.5 openness until back above 0.7: about 2/3 of a sine dip.
        assert e.duration_ms == pytest.approx(dur * 1000 * 2 / 3, abs=40)


def test_a_long_closure_is_not_a_blink_and_one_eye_is_a_wink():
    t = _t(20)
    long_closed = _ear_with_blinks(t, [(5.0, 1.5)])
    o = openness(long_closed, t)
    kinds = [e.kind for e in detect_blinks(o, o, t)]
    assert kinds == ["long_closure"]
    wink = openness(_ear_with_blinks(t, [(5.0, 0.2)]), t)
    other = openness(np.full(len(t), 0.3), t)
    assert detect_blinks(wink, other, t) == []


def test_openness_follows_a_slow_change_in_eye_shape():
    # The eyes narrow (a smile) for 8 s: a lower baseline, not a long blink.
    t = _t(30)
    ear = np.full(len(t), 0.30)
    ear[(t > 10) & (t < 18)] = 0.20
    o = openness(ear, t)
    assert np.nanmin(o[(t > 13) & (t < 15)]) > 0.9
    assert detect_blinks(o, o, t) == []


def test_perclos_is_the_share_of_mostly_closed_time():
    o = np.array([1.0] * 80 + [0.1] * 20)
    assert perclos(o, o, np.ones(100, bool)) == pytest.approx(20.0)
    assert perclos(o, o, np.zeros(100, bool)) is None


def test_hysteresis_needs_the_release_level_to_end():
    x = [1, 0.4, 0.6, 0.4, 0.8, 1]  # dips below 0.5, wobbles, reopens at 0.8
    assert hysteresis_spans(x, 0.5, 0.7, below=True) == [(1, 3)]
    assert hysteresis_spans([1, 0.4, float("nan"), 0.4, 1], 0.5, 0.7, below=True) == [
        (1, 1),
        (3, 3),
    ]


# ── Expression ───────────────────────────────────────────────────────────────


def _shapes(n, **series):
    shapes = {name: np.zeros(n) for name in EXPRESSIVE}
    shapes.update(
        {
            "browInnerUp": np.zeros(n),
            "browOuterUpLeft": np.zeros(n),
            "browOuterUpRight": np.zeros(n),
        }
    )
    for k, v in series.items():
        shapes[k] = np.asarray(v, dtype=np.float64)
    return shapes


def test_smiles_duchenne_and_brow_flashes():
    t = _t(10)
    n = len(t)
    smile = np.where((t > 1) & (t < 3), 0.8, 0.0) + np.where((t > 5) & (t < 6), 0.7, 0.0)
    cheek = np.where((t > 5) & (t < 6), 0.5, 0.0)  # only the second smile reaches the cheeks
    brow = np.where((t > 8) & (t < 8.3), 0.8, 0.0)
    shapes = _shapes(
        n,
        mouthSmileLeft=smile,
        mouthSmileRight=smile,
        cheekSquintLeft=cheek,
        cheekSquintRight=cheek,
        browInnerUp=brow,
        browOuterUpLeft=brow,
        browOuterUpRight=brow,
    )
    events = detect_expression_events(shapes, t)
    kinds = [e.kind for e in events]
    assert kinds.count("smile") == 2
    assert kinds.count("duchenne_smile") == 1
    duchenne = next(e for e in events if e.kind == "duchenne_smile")
    assert 4.9 < duchenne.start_s < 5.1
    brow_ev = next(e for e in events if e.kind == "brow_raise")
    assert brow_ev.extra["flash"] is True


def test_a_brief_twitch_is_not_a_smile():
    t = _t(5)
    smile = np.where((t > 1) & (t < 1.1), 0.9, 0.0)  # 100 ms
    shapes = _shapes(len(t), mouthSmileLeft=smile, mouthSmileRight=smile)
    assert detect_expression_events(shapes, t) == []


def test_expressivity_is_movement_above_the_persons_resting_face():
    t = _t(10)
    n = len(t)
    resting = np.full(n, 0.2)  # this person's resting brow is a bit low
    frown = resting.copy()
    frown[(t > 4) & (t < 5)] = 0.9
    shapes = _shapes(n, browDownLeft=frown, browDownRight=frown)
    e = expressivity(shapes, np.ones(n, bool))
    assert np.nanmax(e[t < 3]) == pytest.approx(0.0)
    assert np.nanmax(e[(t > 4) & (t < 5)]) == pytest.approx(2 * 0.7 / len(EXPRESSIVE))


# ── Head ─────────────────────────────────────────────────────────────────────


def test_a_nod_and_a_shake_are_told_apart():
    t = _t(12)
    yaw = np.zeros(len(t))
    pitch = np.zeros(len(t))
    nod = (t > 2) & (t < 3.2)  # three half-swings of 8 degrees, 0.4 s each
    pitch[nod] = 8.0 * np.sin(2 * math.pi * (t[nod] - 2) / 0.8)
    shake = (t > 7) & (t < 8.6)
    yaw[shake] = 10.0 * np.sin(2 * math.pi * (t[shake] - 7) / 0.8)
    events = detect_head_gestures(yaw, pitch, t)
    kinds = [e.kind for e in events]
    assert kinds == ["nod", "shake"]
    assert 1.8 < events[0].start_s < 2.6 and events[0].extra["swings"] >= 2
    assert 6.8 < events[1].start_s < 7.6


def test_a_slow_turn_and_small_tremor_are_not_gestures():
    t = _t(10)
    yaw = np.linspace(0, 40, len(t))  # turning to look at someone
    pitch = 1.0 * np.sin(2 * math.pi * t / 0.6)  # 1 degree tremor
    assert detect_head_gestures(yaw, pitch, t) == []


def test_a_slow_small_nod_is_found():
    # +-4 degrees at 1 Hz: never more than 0.5 degrees from one frame to the next.
    t = _t(10)
    pitch = np.zeros(len(t))
    nod = (t > 3) & (t < 6)
    pitch[nod] = 4.0 * np.sin(2 * math.pi * (t[nod] - 3))
    events = detect_head_gestures(np.zeros(len(t)), pitch, t)
    assert [e.kind for e in events] == ["nod"]
    assert events[0].extra["swings"] >= 4
    assert 2.9 < events[0].start_s < 3.3


def test_a_diagonal_movement_is_neither_a_nod_nor_a_shake():
    t = _t(6)
    move = (t > 2) & (t < 4)
    wave = np.where(move, 8.0 * np.sin(2 * math.pi * (t - 2)), 0.0)
    assert detect_head_gestures(wave, wave, t) == []


def test_a_nod_is_found_through_landmark_jitter():
    rng = np.random.default_rng(3)
    t = _t(8)
    pitch = rng.normal(0.0, 0.6, len(t))  # frame-to-frame head-pose jitter
    yaw = rng.normal(0.0, 0.6, len(t))
    nod = (t > 3) & (t < 4.2)
    pitch[nod] += 8.0 * np.sin(2 * math.pi * (t[nod] - 3) / 0.8)
    events = detect_head_gestures(smooth(yaw, t), smooth(pitch, t), t)
    assert [e.kind for e in events] == ["nod"]
    assert 2.8 < events[0].start_s < 3.3


def test_smooth_is_a_centred_mean_that_skips_gaps():
    t = _t(1)
    x = np.arange(len(t), dtype=np.float64)
    x[20] = np.nan
    y = smooth(x, t, window_s=0.1)  # 5 or 6 samples wide at 50 fps
    assert y[10] == pytest.approx(10.0)
    assert np.isnan(y[20])
    assert y[21] == pytest.approx(np.nanmean(x[19:24]))


def test_angular_speed_in_degrees_per_second():
    t = _t(2)
    yaw = 30.0 * t  # 30 deg/s
    speed = angular_speed(yaw, np.zeros(len(t)), np.zeros(len(t)), t)
    assert np.isnan(speed[0])
    assert np.nanmedian(speed) == pytest.approx(30.0)


# ── Summary ──────────────────────────────────────────────────────────────────


def test_event_durations_agree_everywhere():
    t = _t(10)
    o = openness(_ear_with_blinks(t, [(2.0, 0.2), (5.0, 0.3)]), t)
    events = detect_blinks(o, o, t)
    s = summarise(events, t, np.ones(len(t), bool), o, o, np.zeros(len(t)), np.zeros(len(t)))
    durations = sorted(e.duration_ms for e in events)
    assert s["blinks"]["median_duration_ms"] == pytest.approx(np.median(durations))
    for e in events:  # whole frames: a multiple of the 20 ms frame interval
        assert e.duration_ms / 20.0 == pytest.approx(round(e.duration_ms / 20.0))


def test_blink_intervals_skip_time_without_a_face():
    t = _t(60)
    o = openness(_ear_with_blinks(t, [(s, 0.2) for s in (5, 9, 13, 40, 44)]), t)
    events = detect_blinks(o, o, t)
    valid = np.ones(len(t), bool)
    valid[(t > 15) & (t < 38)] = False  # looked away
    s = summarise(events, t, valid, o, o, np.zeros(len(t)), np.zeros(len(t)))
    assert s["blinks"]["mean_interval_s"] == pytest.approx(4.0, abs=0.05)


def test_summary_rates_are_per_minute_of_face_seen():
    t = _t(60)
    ear = _ear_with_blinks(t, [(s, 0.2) for s in range(2, 60, 4)])  # 15 blinks
    o = openness(ear, t)
    events = detect_blinks(o, o, t)
    valid = np.ones(len(t), bool)
    valid[: len(t) // 2] = False  # face only seen for 30 s...
    events = [e for e in events if e.start_s >= 30.0]  # ...so only those blinks count
    shapes = _shapes(len(t))
    s = summarise(events, t, valid, o, o, expressivity(shapes, valid), np.zeros(len(t)))
    assert s["face_seen_s"] == pytest.approx(30.0, abs=0.1)
    assert s["blinks"]["count"] == len(events)
    assert s["blinks"]["per_minute"] == pytest.approx(len(events) * 2.0, abs=0.1)
    assert s["blinks"]["mean_interval_s"] == pytest.approx(4.0, abs=0.05)
