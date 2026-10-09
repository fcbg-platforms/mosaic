"""
Tests for the conversation package: speech activity, audio timing, who is
speaking, and turns/transitions, on synthetic signals with known answers.
"""

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from conversation import speakers as spk
from conversation import timing as tm
from conversation import turns as tt
from conversation.vad import HOP_S, band_energy_db, detect_speech, runs

RATE = 16000


# ── Speech activity ──────────────────────────────────────────────────────────


def _speechlike(n, rng):
    """A 200 Hz voice with harmonics, amplitude-modulated at 5 Hz."""
    t = np.arange(n) / RATE
    voice = sum(np.sin(2 * np.pi * 200 * k * t) / k for k in range(1, 8))
    return 0.2 * voice * (0.6 + 0.4 * np.sin(2 * np.pi * 5 * t)) + rng.normal(0, 0.002, n)


def test_speech_is_found_where_it_is_and_hum_is_not_speech():
    rng = np.random.default_rng(1)
    n = 12 * RATE
    t = np.arange(n) / RATE
    x = rng.normal(0, 0.003, n) + 0.3 * np.sin(2 * np.pi * 50 * t)  # loud mains hum
    truth = [(1.0, 3.0), (5.0, 5.8), (8.0, 11.0)]
    for a, b in truth:
        i, j = int(a * RATE), int(b * RATE)
        x[i:j] += _speechlike(j - i, rng)
    mask, levels = detect_speech(band_energy_db(x, RATE))
    found = [(i * HOP_S, j * HOP_S) for i, j in runs(mask)]
    assert len(found) == 3
    for (a, b), (fa, fb) in zip(truth, found, strict=True):
        assert abs(fa - a) < 0.04 and abs(fb - b) < 0.04
    assert levels["on_db"] > levels["noise_db"]


def test_a_click_is_not_speech_and_a_short_gap_is_filled():
    rng = np.random.default_rng(2)
    n = 6 * RATE
    x = rng.normal(0, 0.003, n)
    x[RATE : 2 * RATE] += _speechlike(RATE, rng)
    x[int(2.08 * RATE) : 3 * RATE] += _speechlike(int(0.92 * RATE), rng)  # 80 ms gap
    x[int(4.5 * RATE) : int(4.53 * RATE)] += 0.5  # 30 ms click
    mask, _ = detect_speech(band_energy_db(x, RATE))
    found = [(i * HOP_S, j * HOP_S) for i, j in runs(mask)]
    assert len(found) == 1
    assert found[0][0] == pytest.approx(1.0, abs=0.04) and found[0][1] == pytest.approx(
        3.0, abs=0.04
    )


# ── Audio timing ─────────────────────────────────────────────────────────────


def test_clock_line_is_fitted_under_late_arrivals():
    rng = np.random.default_rng(3)
    rate = 44100
    true_ns_per_sample = 1e9 / rate * (1 + 40e-6)  # sound card 40 ppm slow
    t0 = 5_000_000_000.0
    k = np.cumsum(np.full(3000, 4410))  # a buffer every 100 ms
    arrivals = t0 + k * true_ns_per_sample + rng.exponential(4e6, k.size) + 1e6
    clock = tm.fit_timing(k, arrivals, rate)
    assert clock.method == "timing_file"
    assert clock.ns_per_sample == pytest.approx(true_ns_per_sample, rel=2e-6)
    # Placed at the earliest arrivals: within a couple of ms of the truth.
    assert abs(clock.t0_ns - t0) < 3e6
    assert tm.fit_timing([1, 2], [0, 1e12], rate) is None  # absurd slope


def test_a_dropout_becomes_a_step_not_a_tilt():
    rng = np.random.default_rng(7)
    rate = 44100
    nps = 1e9 / rate
    k = np.cumsum(np.full(36000, 4410))  # an hour of 100 ms buffers
    t = 1e9 + k * nps + 1e6 + rng.exponential(3e6, k.size)
    lost = k > k[k.size // 2]
    t[lost] += 200e6  # 200 ms of samples never reached the file
    clock = tm.fit_timing(k, t, rate)
    assert clock.step_ms == pytest.approx(200, abs=10)
    truth = 1e9 + k * nps + np.where(lost, 200e6, 0.0)
    err_ms = (clock.sample_time_ns(k) - truth) / 1e6
    away = np.abs(k - k[k.size // 2]) > 11 * rate  # outside the step's window
    assert np.abs(err_ms[away]).max() < 3


def test_timing_csv_round_trip(tmp_path):
    p = tmp_path / "audio.timing.csv"
    p.write_text("sample_count,elapsed_ns\n4410,1100000000\n8820,1200000000\nbad,row\n")
    k, t = tm.read_timing_csv(p)
    assert k.tolist() == [4410, 8820] and t.tolist() == [1.1e9, 1.2e9]


def test_audio_video_lag_from_the_mouth():
    rng = np.random.default_rng(4)
    n = 6000  # 60 s on the 10 ms grid
    bursts = np.convolve(rng.random(n) > 0.97, np.ones(30), mode="same") > 0
    mouth = bursts + rng.normal(0, 0.2, n)
    audio = np.roll(bursts.astype(float), 12) + rng.normal(0, 0.2, n)  # audio 120 ms late
    lag, r, prominence = tm.estimate_av_lag(mouth, audio, 0.01)
    assert lag == pytest.approx(0.12, abs=0.011)
    assert tm.lag_is_reliable(r, prominence)
    # Unrelated signals: no reliable lag.
    _, r, prominence = tm.estimate_av_lag(rng.normal(size=n), rng.normal(size=n), 0.01)
    assert not tm.lag_is_reliable(r, prominence)


# ── Who is speaking ──────────────────────────────────────────────────────────


def test_mouth_activity_sees_syllables_not_a_held_open_mouth():
    g = np.arange(0, 10, 0.01)
    frames = np.arange(0, 10, 0.02)  # 50 fps
    mouth = np.where(
        frames < 5, 0.2 + 0.15 * np.sin(2 * np.pi * 5 * frames), 0.6
    )  # talk, then gape
    a = spk.mouth_activity(spk.mouth_on_grid(frames, mouth, g), 0.01)
    assert np.nanmedian(a[(g > 1) & (g < 4)]) > 0.08
    assert np.nanmax(a[g > 6]) < 1e-6


def test_face_gaps_are_not_bridged():
    g = np.arange(0, 3, 0.01)
    frames = np.array([0.0, 0.02, 0.04, 2.0, 2.02])
    m = spk.mouth_on_grid(frames, np.ones(5), g)
    assert np.isfinite(m[2]) and np.isnan(m[100])


def _scene():
    """10 s: the subject talks 1-4 s (mouth moving), the other 5-8 s (mouth
    still), silence elsewhere."""
    g = np.arange(0, 10, 0.01)
    speech = ((g >= 1) & (g < 4)) | ((g >= 5) & (g < 8))
    rng = np.random.default_rng(5)
    activity = np.where((g >= 1) & (g < 4), 0.1, 0.01) + rng.normal(0, 0.003, len(g))
    return g, speech, activity


def test_threshold_falls_back_to_splitting_speech_without_silence():
    g = np.arange(0, 20, 0.01)
    speech = np.ones(len(g), bool)  # wall-to-wall talk, no silence at all
    rng = np.random.default_rng(6)
    subject_talks = (g % 4) < 2
    activity = np.where(subject_talks, 0.1, 0.01) * np.exp(rng.normal(0, 0.2, len(g)))
    thr = spk.speaking_threshold(activity, speech)
    assert 0.015 < thr < 0.07
    assert spk.speaking_threshold(np.full(len(g), 0.05), speech) is None  # one class only


def test_speaking_threshold_comes_from_silence_and_attribution_follows_the_mouth():
    g, speech, activity = _scene()
    thr = spk.speaking_threshold(activity, speech)
    assert 0.01 < thr < 0.03
    vis, strong = spk.visual_speaking(activity, thr, 0.01)
    seen = np.ones(len(g), bool)
    who = spk.attribute(speech, seen, vis, strong)
    assert who["subject"][(g > 1.5) & (g < 3.5)].all()
    assert who["other"][(g > 5.5) & (g < 7.5)].all()
    assert not who["subject"][(g > 5.5) & (g < 7.5)].any()


def test_diarized_labels_are_matched_and_then_decide():
    g, speech, activity = _scene()
    thr = spk.speaking_threshold(activity, speech)
    vis, strong = spk.visual_speaking(activity, thr, 0.01)
    seen = np.ones(len(g), bool)
    segments = [
        {"start_ms": 1000, "end_ms": 4000, "speaker": "SPEAKER_01"},
        {"start_ms": 5000, "end_ms": 8000, "speaker": "SPEAKER_00"},
    ]
    labels = spk.labels_on_grid(segments, g)
    label, scores = spk.match_diarized_speaker(labels, speech, vis, seen)
    assert label == "SPEAKER_01" and scores["SPEAKER_00"] < 0.1
    # Face out of view for the subject's speech: the label still decides.
    unseen = seen & ~((g >= 1) & (g < 4))
    subj_cov = spk.label_coverage(segments, g, {label})
    other_cov = spk.label_coverage(segments, g, {"SPEAKER_00"})
    who = spk.attribute(speech, unseen, vis & unseen, strong & unseen, subj_cov, other_cov)
    assert who["subject"][(g > 1.5) & (g < 3.5)].all()
    assert who["unknown"].sum() == 0


def test_overlapping_diarized_segments_give_overlapping_speech():
    g = np.arange(0, 6, 0.01)
    speech = (g >= 1) & (g < 5)
    segments = [
        {"start_ms": 1000, "end_ms": 3500, "speaker": "A"},
        {"start_ms": 3000, "end_ms": 5000, "speaker": "B"},
    ]
    none = np.zeros(len(g), bool)
    who = spk.attribute(
        speech,
        np.ones(len(g), bool),
        none,
        none,
        spk.label_coverage(segments, g, {"B"}),
        spk.label_coverage(segments, g, {"A"}),
    )
    both = who["subject"] & who["other"]
    assert both[(g > 3.05) & (g < 3.45)].all() and not both[g < 2.9].any()


def test_one_label_cannot_separate_the_speakers():
    g, speech, activity = _scene()
    thr = spk.speaking_threshold(activity, speech)
    vis, _ = spk.visual_speaking(activity, thr, 0.01)
    labels = spk.labels_on_grid([{"start_ms": 1000, "end_ms": 8000, "speaker": "S0"}], g)
    label, scores = spk.match_diarized_speaker(labels, speech, vis, np.ones(len(g), bool))
    assert label is None and list(scores) == ["S0"]


def test_no_match_when_the_mouth_does_not_tell_the_labels_apart():
    g, speech, _ = _scene()
    labels = spk.labels_on_grid(
        [
            {"start_ms": 1000, "end_ms": 4000, "speaker": "A"},
            {"start_ms": 5000, "end_ms": 8000, "speaker": "B"},
        ],
        g,
    )
    still = np.zeros(len(g), bool)
    label, _ = spk.match_diarized_speaker(labels, speech, still, np.ones(len(g), bool))
    assert label is None


# ── Turns ────────────────────────────────────────────────────────────────────


def _masks(subject, other, end=20.0, hop=0.01):
    g = np.arange(0, end, hop)
    s = np.zeros(len(g), bool)
    o = np.zeros(len(g), bool)
    for a, b in subject:
        s[(g >= a - 1e-9) & (g < b - 1e-9)] = True
    for a, b in other:
        o[(g >= a - 1e-9) & (g < b - 1e-9)] = True
    return s, o


def _analyse(subject, other):
    s, o = _masks(subject, other)
    si = tt.merge_close(tt.mask_intervals(s, 0.01), tt.MIN_PAUSE_S)
    oi = tt.merge_close(tt.mask_intervals(o, 0.01), tt.MIN_PAUSE_S)
    spurts = tt.classify_spurts(si, oi)
    turns, transitions = tt.build_turns(spurts)
    return spurts, turns, transitions, tt.overlap_intervals(s, o, 0.01)


def test_gaps_overlaps_pauses_backchannels_and_interruptions():
    subject = [(0.0, 3.0), (4.7, 8.0), (8.6, 10.0)]  # a 0.6 s pause at 8.0
    other = [(3.3, 5.0), (6.0, 6.4), (9.0, 12.0)]  # gap, backchannel, interruption
    spurts, turns, transitions, overlaps = _analyse(subject, other)
    assert [s.kind for s in spurts if s.speaker == tt.OTHER] == ["turn", "backchannel", "turn"]
    assert [t.speaker for t in turns] == ["subject", "other", "subject", "other"]
    ftos = [round(x.fto_s, 2) for x in transitions]
    assert ftos == [0.3, -0.3, -1.0]
    assert [x.interruption for x in transitions] == [False, False, True]
    assert turns[2].pauses == [(pytest.approx(8.0), pytest.approx(8.6))]
    s = tt.summarise(spurts, turns, transitions, overlaps, {"subject": 30})
    subj, oth = s["speakers"]["subject"], s["speakers"]["other"]
    assert subj["turns"] == 2 and oth["turns"] == 2
    assert subj["pauses"] == 1 and oth["backchannels"] == 1
    assert subj["response_offset"]["median_s"] == pytest.approx(-0.3)
    assert oth["interruptions"] == 1
    assert s["transitions"]["gaps"] == 1 and s["transitions"]["overlapping"] == 2
    assert s["overlap"]["count"] == 3  # 4.7-5.0, the backchannel, 9.0-10.0
    assert subj["words_per_min"] == pytest.approx(60 * 30 / (3.0 + 3.3 + 1.4), rel=0.01)


def test_a_long_overlap_inside_a_turn_does_not_take_the_floor():
    spurts, turns, transitions, _ = _analyse([(0.0, 6.0)], [(2.0, 3.5)])
    assert [s.kind for s in spurts] == ["turn", "overlap"]
    assert len(turns) == 1 and transitions == []


def test_a_short_answer_in_silence_is_a_turn():
    spurts, turns, transitions, _ = _analyse([(2.4, 2.8)], [(0.0, 2.0), (3.5, 6.0)])
    assert [t.speaker for t in turns] == ["other", "subject", "other"]
    assert [round(x.fto_s, 2) for x in transitions] == [0.4, 0.7]


def test_summary_of_silence():
    s = tt.summarise([], [], [], [])
    assert s["conversation_s"] == 0.0 and s["transitions"]["count"] == 0
