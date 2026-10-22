"""
Tests for the session report: reading every plugin's output (and tolerating
any missing), the flat summary row, the combined CSV and the HTML page.
"""

import csv
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_session_report
from report import collect, render, summary

T0 = 1_000_000.0  # elapsed ms of the video's first frame


def _write(path: Path, doc) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc), encoding="utf-8")


def make_session(root: Path, *, mode="interview", cams=(2,), full=True) -> Path:
    """A session folder with each plugin's output for its cameras."""
    s = root
    meta = {
        "bids": {"sub": "P01", "ses": "pre", "task": "interview", "run": 1},
        "recorded_by": "tester",
        "session_start_utc": "2026-10-10T09:00:00.000Z",
        "recording": {"mode": mode},
        "cameras": [{"index": c, "name": "acA1920-25gc"} for c in cams],
        "session_end": {
            "utc": "2026-10-10T09:05:00.000Z",
            "duration_ms": 300000,
            "ended_cleanly": True,
        },
    }
    _write(s / "session_meta.json", meta)
    (s / "notes.txt").write_text("Participant arrived late.", encoding="utf-8")
    for c in cams:
        stem = f"video_{c}"
        (s / "video").mkdir(parents=True, exist_ok=True)
        (s / "video" / f"{stem}.mp4").write_bytes(b"")
        with (s / "video" / f"timestamps_cam{c}.csv").open("w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["frame_id", "elapsed_ns", "wall_ns", "hw_timestamp_ns"])
            for k in range(0, 15001):
                w.writerow([k + 1, int((T0 + 20.0 * k) * 1e6), 0, 0])
        if not full:
            continue
        _write(
            s / "face_dynamics" / f"{stem}.face_dynamics.json",
            {
                "schema": "mosaic-face-dynamics-v1",
                "fps": 50.0,
                "summary": {
                    "face_seen_pct": 97.5,
                    "blinks": {
                        "count": 60,
                        "per_minute": 12.3,
                        "median_duration_ms": 160.0,
                        "long_closures": 1,
                        "perclos_pct": 1.2,
                    },
                    "expression": {
                        "smiles": 8,
                        "duchenne_smiles": 3,
                        "smiling_pct": 9.5,
                        "brow_raises": 5,
                        "brow_flashes": 2,
                        "expressivity_mean": 0.05,
                    },
                    "head": {"nods": 14, "shakes": 2},
                },
                "events": [
                    {
                        "kind": "blink",
                        "start_ms": T0 + 5000,
                        "end_ms": T0 + 5160,
                        "peak_ms": T0 + 5080,
                    },
                    {
                        "kind": "smile",
                        "start_ms": T0 + 20000,
                        "end_ms": T0 + 23000,
                        "peak_ms": T0 + 21000,
                    },
                    {
                        "kind": "duchenne_smile",
                        "start_ms": T0 + 20000,
                        "end_ms": T0 + 23000,
                        "peak_ms": T0 + 21000,
                    },
                    {
                        "kind": "nod",
                        "start_ms": T0 + 40000,
                        "end_ms": T0 + 41000,
                        "peak_ms": T0 + 40500,
                    },
                ],
                "frames": [],
            },
        )
        _write(
            s / "conversation" / f"{stem}.conversation.json",
            {
                "schema": "mosaic-conversation-v1",
                "timing": {"method": "timing_file"},
                "attribution": {"method": "diarization"},
                "summary": {
                    "speakers": {
                        "subject": {
                            "speech_s": 150.0,
                            "speech_pct": 55.0,
                            "turns": 20,
                            "turn_duration": {"median_s": 6.0},
                            "response_offset": {"median_s": 0.42},
                            "interruptions": 1,
                            "pauses_per_min": 8.0,
                            "backchannels": 4,
                            "words_per_min": 140.0,
                        },
                        "other": {
                            "speech_s": 90.0,
                            "speech_pct": 33.0,
                            "turns": 20,
                            "turn_duration": {"median_s": 3.0},
                            "response_offset": {"median_s": 0.25},
                            "interruptions": 0,
                        },
                    },
                    "transitions": {"count": 39, "gaps": 30, "overlapping": 9, "interruptions": 1},
                    "overlap": {"count": 12, "total_s": 6.0, "pct_of_speech": 2.5},
                },
                "spurts": [
                    {
                        "speaker": "other",
                        "start_ms": T0 + 1000,
                        "end_ms": T0 + 4000,
                        "kind": "turn",
                    },
                    {
                        "speaker": "subject",
                        "start_ms": T0 + 4400,
                        "end_ms": T0 + 12000,
                        "kind": "turn",
                    },
                ],
                "transitions": [
                    {"to": "subject", "fto_ms": 420},
                    {"to": "other", "fto_ms": -300},
                ],
                "turns": [],
            },
        )
        _write(
            s / "eye_contact" / f"{stem}.eye_contact.json",
            {
                "schema": "mosaic-eye-contact-v1",
                "target": {"method": "auto", "radius_deg": 6.5},
                "summary": {
                    "eye_contact_pct": 62.0,
                    "eye_contact_listening_pct": 81.0,
                    "eye_contact_speaking_pct": 44.0,
                    "aversions": 30,
                    "aversions_per_min": 6.0,
                    "aversion_median_s": 1.4,
                    "aversion_directions": {"up": 12, "down": 10, "left": 5, "right": 3},
                    "turns": {"turn_start_aversion_pct": 70.0, "turn_end_contact_pct": 55.0},
                },
                "aversions": [],
                "frames": [
                    {"frame_index": 0, "timestamp_ms": T0, "contact": True},
                    {"frame_index": 1, "timestamp_ms": T0 + 3000, "contact": False},
                    {"frame_index": 2, "timestamp_ms": T0 + 5000, "contact": None},
                    {"frame_index": 3, "timestamp_ms": T0 + 6000, "contact": True},
                ],
            },
        )
        _write(
            s / "rppg" / f"{stem}.pos.rppg.json",
            {
                "schema": "mosaic-rppg-v2",
                "backend": "pos",
                "summary": {
                    "mean_bpm": 74.0,
                    "min_bpm": 66.0,
                    "max_bpm": 85.0,
                    "pct_windows_good": 0.9,
                },
                "windows": [
                    {"start_ms": T0, "end_ms": T0 + 10000, "bpm": 72.0, "smoothed_bpm": 72.0},
                    {
                        "start_ms": T0 + 2000,
                        "end_ms": T0 + 12000,
                        "bpm": 75.0,
                        "smoothed_bpm": 74.0,
                    },
                ],
                "hrv": {
                    "nn_count": 300,
                    "rmssd_ms": 55.0,
                    "rmssd_corrected_ms": 41.0,
                    "sdnn_ms": 48.0,
                    "sdnn_corrected_ms": 44.0,
                    "timing_jitter_ms": 12.0,
                },
                "hrv_withheld": [],
                "by_state": {"speaking": {"mean_hr_bpm": 78.0}, "listening": {"mean_hr_bpm": 71.0}},
            },
        )
        _write(
            s / "gaze2d" / f"{stem}.gaze2d.json",
            {"summary": {"pct_frames_with_face": 0.95, "pct_on_target": 0.4}},
        )
        _write(
            s / "expression" / f"{stem}.expression.json",
            {
                "backend": "blendshape",
                "frames": [
                    {"subjects": [{"dominant_expression": "neutral"}]},
                    {"subjects": [{"dominant_expression": "neutral"}]},
                    {"subjects": [{"dominant_expression": "happy"}]},
                    {"subjects": []},
                ],
            },
        )
    if full:
        _write(
            s / "audio" / "audio.transcript.json",
            {
                "source_audio": "audio.wav",
                "diarization": True,
                "segments": [
                    {"start_ms": 0, "end_ms": 2000, "speaker": "SPEAKER_00", "text": "How are you"},
                    {
                        "start_ms": 2500,
                        "end_ms": 6000,
                        "speaker": "SPEAKER_01",
                        "text": "Fine thanks a lot",
                    },
                ],
            },
        )
        _write(
            s / "gaze_fusion" / "summary.json",
            {
                "subjects": {"S1": {"seen_s": 200.0, "looked_at_s": {"S2": 80.0}}},
                "mutual_gaze": {"S1 & S2": {"seconds": 30.0, "pct_of_recording": 10.0}},
            },
        )
    return s


def test_collect_reads_every_plugin_and_tolerates_missing(tmp_path):
    full = collect.collect(make_session(tmp_path / "a"))
    (cam,) = full.cameras
    assert cam.name == "Camera 3" and cam.model == "acA1920-25gc"
    assert set(cam.results) == {
        "face_dynamics",
        "conversation",
        "eye_contact",
        "rppg",
        "gaze2d",
        "expression",
    }
    assert cam.t0_ms == pytest.approx(T0) and cam.duration_s == pytest.approx(300.0)
    assert set(full.session_results) == {"transcripts", "gaze_fusion"}
    assert cam.results["expression"]["shares_pct"] == {
        "neutral": pytest.approx(66.7),
        "happy": pytest.approx(33.3),
    }
    runs = cam.results["eye_contact"]["runs"]
    # Unknown stretches (no gaze) are not drawn: the away run ends where it starts.
    assert [r[2] for r in runs] == [True, False, True]
    assert runs[1][1] == pytest.approx(T0 + 5000)
    assert full.session_results["transcripts"][0]["words"] == 7
    empty = collect.collect(make_session(tmp_path / "b", full=False))
    assert empty.cameras[0].results == {} and empty.session_results == {}
    assert collect.duration_s(empty) == 300.0


def test_interview_row_is_unprefixed_and_room_rows_are_per_camera(tmp_path):
    row = summary.session_row(collect.collect(make_session(tmp_path / "a")))
    assert row["subject"] == "P01" and row["mode"] == "interview" and row["duration_s"] == 300.0
    assert row["fd_blinks_per_min"] == 12.3 and row["conv_subject_response_ms"] == 420
    assert row["ec_listening_pct"] == 81.0 and row["hrv_rmssd_corrected_ms"] == 41.0
    assert row["hr_speaking_bpm"] == 78.0 and row["gaze2d_on_target_pct"] == 40.0
    assert row["expr_dominant"] == "neutral" and row["transcript_words"] == 7
    assert row["gaze3d_mutual_S1_S2_pct"] == 10.0  # by subject id, not display name
    room = summary.session_row(
        collect.collect(make_session(tmp_path / "r", mode="room", cams=(0, 5)))
    )
    assert "cam1_fd_blinks_per_min" in room and "cam6_ec_contact_pct" in room
    assert "fd_blinks_per_min" not in room


def test_html_page_has_every_section_and_is_self_contained(tmp_path):
    data = collect.collect(make_session(tmp_path / "a"))
    page = render.render(data)
    for text in (
        "Session report",
        "subject P01",
        "Participant arrived late.",
        "At a glance",
        "Face Dynamics",
        "Conversation Timing",
        "Eye Contact",
        "Heart rate and HRV",
        "Transcript",
        "3D Gaze",
        "420 ms",
        "<svg",
        "Not run on this session",
        "Data quality",
        "Response times",
        "Eye contact while",
        "Fine thanks a lot",  # the transcript itself
        "<nav class='toc'>",
    ):
        assert text in page, text
    assert "Voice analysis" in page and "Pose" in page  # listed as not run
    assert "<script" not in page and "http" not in page  # nothing external
    assert "<b>On camera</b>" in page


def test_a_camera_that_saw_nothing_is_one_line(tmp_path):
    s = make_session(tmp_path / "a", cams=(0, 1))
    fd = json.loads((s / "face_dynamics" / "video_0.face_dynamics.json").read_text())
    fd["summary"]["face_seen_pct"] = 0.0
    (s / "face_dynamics" / "video_0.face_dynamics.json").write_text(json.dumps(fd))
    for sub in ("conversation", "eye_contact", "gaze2d", "expression"):
        next((s / sub).glob("video_0.*")).unlink()
    (s / "rppg" / "video_0.pos.rppg.json").unlink()
    page = render.render(collect.collect(s))
    assert "Camera 1: analysed, but no face" in page
    assert page.count("id='cam1'>Camera 2") == 1 and "id='cam0'>Camera 1" not in page


def test_runner_writes_report_and_combines_sessions(tmp_path):
    root = tmp_path / "recordings"
    make_session(root / "tester" / "s1")
    make_session(root / "tester" / "s2", full=False)
    assert run_session_report.main(["--session", str(root / "tester" / "s1")]) == 0
    out = root / "tester" / "s1" / "report"
    assert (out / "session_report.html").exists() and (out / "session_summary.csv").exists()
    assert json.loads((out / "session_summary.json").read_text())["fd_smiles"] == 8
    assert run_session_report.main(["--sessions-root", str(root), "--summary-only"]) == 0
    rows = list(csv.DictReader((root / "sessions_summary.csv").open(encoding="utf-8-sig")))
    assert [r["session"] for r in rows] == ["s1", "s2"]
    assert rows[0]["fd_smiles"] == "8" and rows[1]["fd_smiles"] == ""
    assert run_session_report.main(["--session", str(tmp_path)]) == 1  # not a session


def test_damaged_inputs_do_not_stop_the_report(tmp_path):
    s = make_session(tmp_path / "a")
    meta = json.loads((s / "session_meta.json").read_text())
    meta["session_end"] = None  # never finished
    (s / "session_meta.json").write_text(json.dumps(meta))
    with (s / "video" / "timestamps_cam2.csv").open("a") as f:
        f.write("15002")  # cut off mid-row by a crash
    (s / "notes.txt").write_bytes("très fatigué".encode("cp1252"))
    data = collect.collect(s)
    assert data.cameras[0].duration_s == pytest.approx(300.0)
    assert collect.duration_s(data) == pytest.approx(300.0)
    page = render.render(data)
    assert "did not finish" in page and "tr" in data.notes
    # No timestamps at all: no timeline, but the tables still come.
    (s / "video" / "timestamps_cam2.csv").unlink()
    page = render.render(collect.collect(s))
    assert 'aria-label="Timeline of' not in page and "Face Dynamics" in page


def test_rmssd_tile_keeps_a_corrected_zero_and_odd_values_are_escaped(tmp_path):
    s = make_session(tmp_path / "a")
    path = s / "rppg" / "video_2.pos.rppg.json"
    d = json.loads(path.read_text())
    d["hrv"]["rmssd_corrected_ms"] = 0.0  # all of it was timing noise
    d["backend"] = "<b>pos</b>"
    path.write_text(json.dumps(d))
    page = render.render(collect.collect(s))
    assert "0 ms</div><div class='k'>RMSSD (noise-corrected)" in page
    assert "<b>pos</b>" not in page
    assert render.num("<img src=x>") == "&lt;img src=x&gt;"


def test_pose_voice_and_sync_readers(tmp_path):
    s = make_session(tmp_path / "a")
    frames = [{"subjects": [{"subject_id": 1}, {"subject_id": -1}]} for _ in range(100)]
    frames[0]["subjects"].append({"subject_id": 7})  # a one-frame track switch
    _write(
        s / "pose" / "video_2.yolov8n-pose.pose.json",
        {"model": "yolov8n-pose.pt", "tracker": "bytetrack", "frames": frames},
    )
    _write(
        s / "audio" / "audio.voice.json",
        {
            "source_audio": "audio.wav",
            "duration_ms": 60000,
            "pitch": {"values_hz": [0, 120, 130, 0]},
            "intensity": {"values_db": [60, 70]},
        },
    )
    _write(
        s / "synced" / "sync_repair.json",
        {
            "alignment": "trigger_ticks",
            "master_fps": 50,
            "duration_ms": 300000,
            "cameras": [
                {
                    "index": 2,
                    "skipped": False,
                    "output_frame_count": 1000,
                    "missing_frame_count": 30,
                    "gap_count": 2,
                }
            ],
        },
    )
    _write(
        s / "skeleton3d.json",
        {  # in run_pose3d.py's order: quality before the (large) frames
            "quality": {
                "identity": {"track_count": 2, "jumps": 1, "swaps": 0, "short_tracks": 0},
                "views": {"2": {"used": 90, "dropped": 10, "unresolved": 3, "dropped_pct": 10.0}},
                "timeline": {
                    "window_s": 1.0,
                    "complete_pct": 50.0,
                    "seconds_by_cause": {"single_view_only": 1.0, "no_frames": 0.0},
                    "windows": [
                        {"t0_s": 0.0, "reconstructed": 2, "cause": None},
                        {
                            "t0_s": 1.0,
                            "reconstructed": 1,
                            "cause": "single_view_only",
                        },
                    ],
                },
            },
            "frames": [{}, {}],
        },
    )
    voice = json.loads((s / "audio" / "audio.voice.json").read_text())
    voice["spectrogram"] = {"t0_ms": 0.0, "t1_ms": 60000.0, "f0_hz": 0.0, "f1_hz": 5000.0}
    voice["pitch"].update({"t0_ms": 0.0, "dt_ms": 10.0, "floor_hz": 75.0, "ceiling_hz": 500.0})
    _write(s / "audio" / "audio.voice.json", voice)
    cv2 = pytest.importorskip("cv2")
    import numpy as np

    cv2.imwrite(str(s / "audio" / "audio.voice.png"), np.zeros((8, 16), np.uint8))
    data = collect.collect(s)
    pose = data.cameras[0].results["pose"]
    assert pose["tracked_people"] == 1 and pose["frames_with_person_pct"] == 100.0
    assert (
        data.session_results["voice"][0]["median_pitch_hz"] == 130
        and data.session_results["voice"][0]["voiced_pct"] == 50.0
    )
    assert data.session_results["sync_repair"]["alignment"] == "trigger_ticks"
    assert "pose" not in collect.collect(s, for_summary_only=True).cameras[0].results
    page = render.render(data)
    assert "Frame sync" in page and "Voice" in page and "People tracked" in page
    # Data quality: missing frames, 3D identities and per-camera disagreement.
    assert "<span class='dot bad'></span>Camera 3 frames</td><td>30 of 1000 missing (3.0%)" in page
    assert "1 jump(s)" in page and "dropped as disagreeing from 10.0%" in page
    assert (
        "everyone seen in 3D in 50% of the seconds with someone in view; people missing: "
        "only one camera saw anyone 1 s" in page
    )
    assert 'aria-label="3D pose coverage"' in page and "var(--q3)" in page
    assert 'aria-label="Spectrogram of audio.wav"' in page and "data:image/png;base64" in page
    assert "pose3d" not in collect.collect(s, for_summary_only=True).session_results
    assert data.session_results["pose3d"]["quality"]["identity"]["jumps"] == 1


def test_still_frames_only_when_asked(tmp_path):
    cv2 = pytest.importorskip("cv2")
    import numpy as np

    s = make_session(tmp_path / "a")
    video = s / "video" / "video_2.mp4"
    out = cv2.VideoWriter(str(video), cv2.VideoWriter_fourcc(*"mp4v"), 25.0, (64, 48))
    for k in range(50):
        out.write(np.full((48, 64, 3), k * 5, np.uint8))
    out.release()
    if video.stat().st_size == 0:
        pytest.skip("this OpenCV cannot write mp4")
    assert "image/jpeg" not in render.render(collect.collect(s))
    data = collect.collect(s, stills=True)
    assert len(data.cameras[0].stills) == 3
    page = render.render(data)
    assert page.count("data:image/jpeg;base64") == 3 and "show the participants" in page


def test_sessions_below_a_folder_named_report_and_several_roots(tmp_path):
    root = tmp_path / "report" / "recordings"  # "report" above the root is fine
    make_session(root / "alice" / "s1")
    make_session(tmp_path / "other" / "bob" / "s2", full=False)
    assert run_session_report.find_sessions(root) == [root / "alice" / "s1"]
    assert (
        run_session_report.main(
            ["--sessions-root", str(root), str(tmp_path / "other"), "--summary-only"]
        )
        == 0
    )
    text = (root / "sessions_summary.csv").read_text(encoding="utf-8-sig")
    rows = list(csv.DictReader(text.splitlines()))
    assert sorted(r["session"] for r in rows) == ["s1", "s2"]


def test_columns_are_grouped_by_camera(tmp_path):
    out = tmp_path / "cols.csv"
    summary.write_rows(out, [{"session": "a", "cam2_x": 1, "cam1_x": 2, "y": 3}])
    assert out.read_text(encoding="utf-8-sig").splitlines()[0] == "session,y,cam1_x,cam2_x"


def test_quality_rows_timeline_tiers_and_response_bins(tmp_path):
    s = make_session(tmp_path / "a")
    meta = json.loads((s / "session_meta.json").read_text())
    meta["cameras"][0]["calibration"] = {
        "calibrated": True,
        "rms_error": 0.42,
        "distortion_model": "k1 k2",
        "holdout_rms_error": 0.47,
        "extrinsic_calibrated": True,
    }
    _write(s / "session_meta.json", meta)
    hr = json.loads((s / "rppg" / "video_2.pos.rppg.json").read_text())
    hr["windows"][0].update({"snr_db": 6.0, "valid_frame_fraction": 0.95})  # clear
    hr["windows"][1].update({"snr_db": -8.0, "valid_frame_fraction": 0.95})  # poor
    _write(s / "rppg" / "video_2.pos.rppg.json", hr)
    data = collect.collect(s)
    rows = {what: (st, detail) for st, what, detail in render.quality_rows(data)}
    assert rows["Camera 3 calibration"] == (
        "ok",
        "lens 0.42 px, k1 k2 (held-out 0.47 px); room calibrated",
    )
    assert rows["Camera 3 heart-rate signal"] == ("warn", "50% of windows clear or good")
    page = render.render(data)
    assert 'stroke="var(--q1)"' in page and 'stroke="var(--q4)"' in page
    assert all(f"href='#{i}'" in page and f"id='{i}'" in page for i in ("quality", "cam2"))
    # 420 ms falls in the 0.25-0.5 s bin, -300 ms in -0.5 to -0.25 s.
    conv = data.cameras[0].results["conversation"]
    assert render.rppg_tier(None, 0.5) == 4 and render.rppg_tier(-2.0, 0.9) == 3
    svg = render._response_chart(conv)
    assert svg.count("<rect") == 2
    # A calibration written with a null error must not stop the report.
    meta["cameras"][0]["calibration"]["rms_error"] = None
    _write(s / "session_meta.json", meta)
    assert "lens calibrated, k1 k2" in render.render(collect.collect(s))


def test_social_section_in_room_sessions_only(tmp_path):
    s = make_session(tmp_path / "a", mode="room", cams=(1, 4))
    _write(
        s / "social" / "social.json",
        {
            "schema": "mosaic-social-v1",
            "cameras": {"a": 1, "b": 4},
            "synchrony": {
                "head": {
                    "summary": {
                        "mean_peak_r": 0.62,
                        "surrogate_mean_peak_r": 0.41,
                        "p_value": 0.01,
                        "a_leads_pct": 80.0,
                        "b_leads_pct": 20.0,
                        "median_lag_s": 0.8,
                    },
                    "series": {"t_s": [1.0]},
                }
            },
            "mimicry": {
                "smile": {
                    "a_to_b": {"events": 10, "rate": 0.6, "chance_rate": 0.3, "p_value": 0.02},
                    "b_to_a": {"events": 0, "rate": None},
                }
            },
            "notes": ["Heart rate: run Remote Heart Rate on Camera 2 and Camera 5."],
        },
    )
    page = render.render(collect.collect(s))
    assert "(A: Camera 2, B: Camera 5)" in page
    assert "peak r 0.62 vs 0.41 by chance (p = 0.01); A leads 80%" in page
    assert "Smile mimicry, B after A" in page and "60% of 10 (chance 30%" in page
    assert "Smile mimicry, A after B" not in page and "run Remote Heart Rate" in page
    # An interview session never lists it as not run.
    one = render.render(collect.collect(make_session(tmp_path / "b")))
    assert "<li>Social Interaction</li>" not in one


def test_objects_card(tmp_path):
    s = make_session(tmp_path / "a")
    _write(
        s / "objects" / "video_2.objects.json",
        {
            "schema": "mosaic-objects-v1",
            "labels": ["toy", "cup"],
            "pose_used": True,
            "summary": {
                "objects": [
                    {"id": 1, "label": "cup", "seen_pct": 93.3, "touched_s_by_person": {"1": 0.8}},
                    {
                        "id": 2,
                        "label": "toy",
                        "name": "red <ball>",  # named in the app; escaped on the page
                        "seen_pct": 20.0,
                        "touched_s_by_person": {},
                    },
                ]
            },
            "frames": [{"objects": [{"polygon": [[0, 0]] * 50}]}],
        },
    )
    page = render.render(collect.collect(s))
    assert "Objects (SAM 3)" in page and "seen 93%; touched by person 1 0.8 s" in page
    assert "seen 20%; not touched" in page
    assert "red &lt;ball&gt; (toy 2)" in page
    assert "objects" not in collect.collect(s, for_summary_only=True).cameras[0].results


def test_objects_card_names_an_unplaced_hand_someone(tmp_path):
    s = make_session(tmp_path / "a")
    _write(
        s / "objects" / "video_2.objects.json",
        {
            "schema": "mosaic-objects-v1",
            "labels": ["toy"],
            "pose_used": False,
            "hands_followed": True,
            "summary": {
                "objects": [
                    {"id": 1, "label": "toy", "seen_pct": 90, "touched_s_by_person": {"?": 2}}
                ]
            },
        },
    )
    page = render.render(collect.collect(s))
    assert "touched by someone 2.0 s" in page and "whose hand it is" in page


def test_clap_check_row(tmp_path):
    s = make_session(tmp_path / "a")
    _write(
        s / "sync_check" / "clap_check.json",
        {
            "schema": "mosaic-clap-check-v1",
            "verdict": "check",
            "cameras": [
                {
                    "camera": 2,
                    "frame_ms": 40.0,
                    "microphones": [
                        {
                            "file": "audio.wav",
                            "matched": 3,
                            "median_offset_ms": 31.0,
                            "spread_ms": 2.0,
                            "verdict": "check",
                        }
                    ],
                }
            ],
        },
    )
    page = render.render(collect.collect(s))
    assert "Audio vs Camera 3" in page and "audio +31 ms from the video over 3 clap(s)" in page
    assert "<span class='dot warn'></span>Audio vs Camera 3" in page
