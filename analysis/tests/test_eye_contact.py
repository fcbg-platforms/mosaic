"""
Tests for eye_contact: gaze angle conventions, the partner's direction, the
contact cone, look-aways and their timing against turns.
"""

import math
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from eye_contact import metrics as ec
from eye_contact.gaze import direction_from_angles, gaze_angles

FPS = 50.0


def _t(seconds):
    return np.arange(int(seconds * FPS)) / FPS


def test_gaze_angle_conventions():
    assert gaze_angles([0.0, 0.0, -1.0]) == pytest.approx((0.0, 0.0))  # straight at the camera
    yaw, pitch = gaze_angles([math.sin(math.radians(20)), 0.0, -math.cos(math.radians(20))])
    assert yaw == pytest.approx(20.0) and pitch == pytest.approx(0.0)  # towards image right
    yaw, pitch = gaze_angles([0.0, -math.sin(math.radians(10)), -math.cos(math.radians(10))])
    assert pitch == pytest.approx(10.0)  # up
    d = direction_from_angles(-15.0, 7.0)
    assert gaze_angles(d) == pytest.approx((-15.0, 7.0))
    assert ec.to_direction([-15.0], [7.0])[0] == pytest.approx(d)


def test_the_mode_finds_the_partner_not_the_mean():
    rng = np.random.default_rng(0)
    # 60% of the time on the partner at (12, -4), the rest scattered widely.
    n = 5000
    on = rng.random(n) < 0.6
    yaw = np.where(on, 12 + rng.normal(0, 2.0, n), rng.uniform(-40, 40, n))
    pitch = np.where(on, -4 + rng.normal(0, 2.0, n), rng.uniform(-30, 30, n))
    my, mp = ec.find_mode(yaw, pitch)
    assert my == pytest.approx(12, abs=0.5) and mp == pytest.approx(-4, abs=0.5)
    assert abs(np.mean(yaw) - 12) > 3  # the mean would be pulled away
    off = ec.angle_between(ec.to_direction(yaw, pitch), ec.to_direction(my, mp))
    radius, sigma, inside = ec.contact_radius(off)
    assert sigma == pytest.approx(2.0, rel=0.25)
    assert radius == pytest.approx(5.0, abs=1.5)
    assert 0.55 < inside < 0.7
    assert ec.find_mode([1.0] * 3, [1.0] * 3) is None


def test_contact_radius_limits():
    assert ec.contact_radius(np.full(100, 0.1))[0] == ec.MIN_RADIUS_DEG
    assert ec.contact_radius(np.full(100, 9.0))[0] == ec.MAX_RADIUS_DEG


def test_states_fill_blinks_and_ignore_glances():
    t = _t(10)
    off = np.full(len(t), 2.0)
    off[(t >= 2.0) & (t < 2.2)] = np.nan  # a blink while looking at the partner
    off[(t >= 4.0) & (t < 4.2)] = 20.0  # a 200 ms glance away: not an aversion
    off[(t >= 6.0) & (t < 8.0)] = 25.0  # a 2 s look-away...
    off[(t >= 7.0) & (t < 7.1)] = 2.0  # ...with a 100 ms flick back inside it
    s = ec.contact_states(off, 6.0, t)
    assert np.all(s[(t >= 2.0) & (t < 2.2)] == 1)
    assert np.all(s[(t >= 4.0) & (t < 4.2)] == 1)
    assert np.all(s[(t >= 6.0) & (t < 8.0)] == 0)
    off[(t >= 9.0)] = np.nan  # face lost at the end: unknown
    assert np.isnan(ec.contact_states(off, 6.0, t)[-1])


def test_aversions_have_durations_and_directions():
    t = _t(12)
    yaw = np.full(len(t), 10.0)
    pitch = np.full(len(t), 0.0)
    pitch[(t >= 2) & (t < 3)] = 25.0  # up
    yaw[(t >= 5) & (t < 5.5)] = 30.0  # towards image right: the subject's left
    pitch[(t >= 8) & (t < 10)] = -20.0  # down
    off = ec.angle_between(ec.to_direction(yaw, pitch), ec.to_direction(10.0, 0.0))
    s = ec.contact_states(off, 6.0, t)
    av = ec.aversions(s, t, yaw, pitch, 10.0, 0.0)
    assert [a["direction"] for a in av] == ["up", "left", "down"]
    assert [round(a["duration_s"], 2) for a in av] == [1.0, 0.5, 2.0]
    assert av[0]["offset_deg"] == pytest.approx(25.0, abs=0.5)
    summ = ec.summarise(s, t, av)
    assert summ["aversions"] == 3 and summ["aversion_directions"]["up"] == 1
    assert summ["eye_contact_pct"] == pytest.approx(100 * (12 - 3.5) / 12, abs=0.5)
    assert summ["aversions_per_min"] == pytest.approx(15.0, abs=0.1)


def test_turn_patterns_and_speaking_states():
    t = _t(30)
    off = np.full(len(t), 2.0)
    off[(t >= 5.2) & (t < 6.5)] = 20.0  # looks away as the answer starts...
    off[(t >= 4.4) & (t < 5.0)] = 20.0  # ...and while thinking before it
    off[(t >= 20.3) & (t < 21.0)] = 20.0  # second answer: away at the start
    off[(t >= 23.5) & (t < 25.0)] = 20.0  # ...and still away at its end
    s = ec.contact_states(off, 6.0, t)
    av = ec.aversions(s, t, np.where(off > 6, 20.0, 0.0), np.zeros(len(t)), 0.0, 0.0)
    turns = [(5.0, 12.0), (20.0, 25.0), (28.0, 29.0)]  # the last is too short to count
    gaps = [(4.0, 5.0), (19.8, 20.0)]
    p = ec.turn_patterns(s, t, av, turns, gaps)
    assert p["subject_turns"] == 2
    assert p["turn_start_aversion_pct"] == 100.0
    assert p["turn_end_contact_pct"] == 50.0
    assert p["gap_aversion_pct"] == 50.0
    speaking = ec.mask_from_intervals(t, turns)
    listening = ec.mask_from_intervals(t, [(0.0, 4.0), (13.0, 19.8)])
    summ = ec.summarise(s, t, av, speaking, listening)
    assert summ["eye_contact_listening_pct"] == 100.0
    assert summ["eye_contact_speaking_pct"] < 80.0


def _track(n, yaw, pitch):
    import run_eye_contact as rec

    g = rec.GazeTrack(n)
    g.yaw[:] = yaw
    g.pitch[:] = pitch
    g.face[:] = np.isfinite(yaw)
    g.origin[:] = [30.0, -40.0, 900.0]
    return g


def _args(**kw):
    import argparse

    a = dict(target="auto", target_yaw=0.0, target_pitch=0.0, radius=None)
    a.update(kw)
    return argparse.Namespace(**a)


def test_analysis_finds_the_partner_from_listening_and_splits_by_state():
    import run_eye_contact as rec

    rng = np.random.default_rng(1)
    t = _t(120)
    n = len(t)
    listening = ((t // 10) % 2) == 0  # 10 s listening, 10 s answering, ...
    # Listening: mostly at the interviewer (25, -5). Answering: half away.
    away = ~listening & (((t % 10) < 3) | ((t % 10) > 6))
    yaw = np.where(away, 20.0, 25.0) + rng.normal(0, 1.0, n)
    pitch = np.where(away, 20.0, -5.0) + rng.normal(0, 1.0, n)  # up and away
    yaw[(t > 50) & (t < 50.15)] = np.nan  # a blink
    times_ms = 1_000_000.0 + t * 1000.0
    spans = [(a, a + 10.0) for a in np.arange(0, 120, 20)]
    answers = [(a + 10.0, a + 20.0) for a in np.arange(0, 120, 20)]
    ms = lambda iv: [(1_000_000.0 + a * 1000, 1_000_000.0 + b * 1000) for a, b in iv]  # noqa: E731
    conv = {
        "other": ms(spans),
        "subject": ms(answers),
        "turns": ms(answers),
        "gaps": ms([(b, b + 0.0) for _, b in spans]),
    }
    a = rec.analyse(_track(n, yaw, pitch), times_ms, conv, _args())
    assert a["target"]["from"] == "listening"
    assert a["target"]["yaw"] == pytest.approx(25, abs=0.5)
    assert a["target"]["pitch"] == pytest.approx(-5, abs=0.5)
    s = a["summary"]
    assert s["eye_contact_listening_pct"] > 95
    assert 25 < s["eye_contact_speaking_pct"] < 35  # 3 of every 10 s
    assert s["aversions"] == 12
    assert s["aversion_directions"]["up"] == 12
    assert s["turns"]["turn_start_aversion_pct"] == 100.0
    assert s["turns"]["turn_end_contact_pct"] == 0.0


def test_analysis_with_the_camera_as_the_partner():
    import run_eye_contact as rec

    t = _t(30)
    g = _track(len(t), np.zeros(len(t)), np.zeros(len(t)))
    # Looking exactly from the eyes to the camera: direction -origin.
    from eye_contact.gaze import gaze_angles

    y, p = gaze_angles(-g.origin[0] / np.linalg.norm(g.origin[0]))
    g.yaw[:] = y
    g.pitch[:] = p
    g.yaw[(t > 10) & (t < 12)] = y + 30
    a = rec.analyse(g, 1e6 + t * 1000.0, None, _args(target="camera"))
    assert a["summary"]["aversions"] == 1
    assert a["summary"]["aversion_directions"]["left"] == 1
    assert "turns" not in a["summary"]


def test_a_blink_beside_a_glitch_is_still_filled():
    t = _t(4)
    off = np.full(len(t), 2.0)
    blink = (t >= 1.0) & (t < 1.1)
    glitch = (t >= 1.1) & (t < 1.2)  # the iris jumps as the lid opens
    off[blink] = np.nan
    off[glitch] = 20.0
    s = ec.contact_states(off, 6.0, t)
    assert np.all(s[blink | glitch] == 1)
    # A short "away" with no contact beside it stays away.
    off2 = np.full(len(t), np.nan)
    off2[(t >= 1.0) & (t < 1.1)] = 20.0
    assert np.all(ec.contact_states(off2, 6.0, t)[(t >= 1.0) & (t < 1.1)] == 0)


def test_contact_states_scale_to_an_hour():
    import time

    rng = np.random.default_rng(2)
    t = np.arange(180_000) / FPS
    off = 6.0 + rng.normal(0, 2.0, len(t))  # flickering at the cone's edge
    off[rng.random(len(t)) < 0.01] = np.nan
    start = time.perf_counter()
    s = ec.contact_states(off, 6.0, t)
    ec.aversions(s, t, off, np.zeros(len(t)), 0.0, 0.0)
    assert time.perf_counter() - start < 5.0


def test_camera_partner_uses_a_fixed_cone():
    import run_eye_contact as rec
    from eye_contact.gaze import gaze_angles

    t = _t(30)
    g = _track(len(t), np.zeros(len(t)), np.zeros(len(t)))
    y, p = gaze_angles(-g.origin[0] / np.linalg.norm(g.origin[0]))
    g.yaw[:] = y + 11.0  # steadily 11 degrees beside the camera
    g.pitch[:] = p
    a = rec.analyse(g, 1e6 + t * 1000.0, None, _args(target="camera"))
    assert a["target"]["radius_deg"] == rec.FIXED_RADIUS_DEG
    assert a["target"]["spread_deg"] is None
    assert a["summary"]["eye_contact_pct"] == 0.0


def test_too_little_gaze_gives_an_empty_result(tmp_path):
    import run_eye_contact as rec

    t = _t(10)
    g = _track(len(t), np.full(len(t), np.nan), np.full(len(t), np.nan))
    with pytest.raises(rec.NoGaze):
        rec.analyse(g, 1e6 + t * 1000.0, None, _args())
    a = rec.empty_analysis(g, 1e6 + t * 1000.0, _args())
    path = rec.write_outputs(
        tmp_path, Path("video_0.mp4"), 0, 50.0, 1e6 + t * 1000.0, g, a, {}, None
    )
    import json

    doc = json.loads(path.read_text())
    assert doc["summary"]["eye_contact_pct"] is None and doc["aversions"] == []
