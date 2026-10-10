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
                "transitions": [],
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
    assert page.count("<h2>Camera 2") == 1 and "<h2>Camera 1" not in page


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
    assert "<svg" not in page and "Face Dynamics" in page


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
            "cameras": [{"index": 2, "skipped": False}],
        },
    )
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
