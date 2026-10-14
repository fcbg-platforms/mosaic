"""
run_session_check.py / session_check: synthetic sessions with known
outcomes (clean, missed frames, interrupted, interview mode, audio stall).
Standard library only.
"""

import json
import sys
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_session_check
from session_check import check_session, find_sessions

FPS = 25.0


def make_session(
    root: Path,
    name="sub-01_ses-1_run-01",
    cams=(0, 1),
    frames=500,
    missed=None,
    ended=True,
    mode="room",
    margin=0.85,
    sync=True,
    exposure=True,
    audio_stall=False,
    start="2026-10-14T10:00:00.000Z",
) -> Path:
    """A session as MOSAIC writes it; `missed` maps camera -> missing frames."""
    missed = missed or {}
    s = root / name
    (s / "video").mkdir(parents=True)
    meta = {
        "schema": "mosaic-session-v1",
        "session_start_utc": start,
        "recording": {"mode": mode, "video_enabled": True, "audio_enabled": True},
        "session_end": {"utc": start, "duration_ms": frames / FPS * 1000, "ended_cleanly": True}
        if ended
        else None,
        "cameras": [],
    }
    ticks = frames
    for i in cams:
        got = frames - missed.get(i, 0)
        meta["cameras"].append(
            {
                "index": i,
                "fps": FPS,
                "frames_recorded": got,
                "achieved_fps": round(FPS * got / frames, 2),
                "pixel_format": "BayerRG8",
                "pixel_format_requested": "BayerRG8",
            }
        )
        cols = "frame_id,elapsed_ns,wall_ns,hw_timestamp_ns" + (",exposure_us" if exposure else "")
        lines = [cols] + [
            f"{k + 1},{k * 40_000_000},0,0" + (",9000" if exposure else "") for k in range(got)
        ]
        (s / "video" / f"timestamps_cam{i}.csv").write_text("\n".join(lines) + "\n")
    (s / "session_meta.json").write_text(json.dumps(meta))
    if mode == "room":
        (s / "video" / "action_group.json").write_text(
            json.dumps({"tick_log": "action_ticks.csv", "cameras": list(cams), "margin": margin})
        )
        period = 1e9 / (FPS * margin)
        (s / "video" / "action_ticks.csv").write_text(
            "tick,elapsed_ns,fired\n"
            + "".join(f"{k},{round(k * period)},{len(cams)}\n" for k in range(ticks))
        )
        if sync:
            (s / "synced").mkdir()
            (s / "synced" / "sync_repair.json").write_text(
                json.dumps(
                    {
                        "schema": "mosaic-sync-repair-v1",
                        "alignment": "trigger_ticks",
                        "total_ticks": ticks,
                        "cameras": [
                            {
                                "index": i,
                                "skipped": False,
                                "output_frame_count": ticks,
                                "missing_frame_count": missed.get(i, 0),
                            }
                            for i in cams
                        ],
                    }
                )
            )
    (s / "audio").mkdir()
    (s / "audio" / "audio.wav").write_bytes(b"RIFF")
    steps = [20_000_000] * 300
    if audio_stall:
        steps[150] = 900_000_000
    t, rows = 0, ["sample_count,elapsed_ns"]
    for k, dt in enumerate(steps):
        t += dt
        rows.append(f"{(k + 1) * 960},{t}")
    (s / "audio" / "audio.timing.csv").write_text("\n".join(rows) + "\n")
    return s


def levels(sc):
    return {i.label: i.level for i in sc.items}


def test_a_clean_triggered_session_passes(tmp_path):
    sc = check_session(make_session(tmp_path))
    assert sc.result == "PASS", sc.items
    assert sc.ended == "clean" and sc.mode == "room"
    assert sc.margin == pytest.approx(0.85)
    assert sc.tick_rate == pytest.approx(FPS * 0.85, rel=1e-3)
    assert [c.missed for c in sc.cameras] == [0, 0]
    assert levels(sc)["Synced videos"] == "PASS"
    assert levels(sc)["Exposure column"] == "PASS"
    assert levels(sc)["Audio (audio.wav)"] == "PASS"


def test_missed_frames_warn_a_little_and_fail_a_lot(tmp_path):
    sc = check_session(make_session(tmp_path, missed={1: 5}))  # 1%
    assert levels(sc)["Camera 2"] == "WARN"
    assert sc.cameras[1].missed_from == "sync repair"
    sc = check_session(make_session(tmp_path / "b", missed={1: 50}))  # 10%
    assert levels(sc)["Camera 2"] == "FAIL"
    assert sc.result == "FAIL"


def test_without_sync_repair_ticks_are_counted_with_end_allowance(tmp_path):
    sc = check_session(make_session(tmp_path, missed={0: 2}, sync=False))
    assert sc.cameras[0].missed_from == "ticks"
    assert levels(sc)["Camera 1"] == "PASS"  # two ticks: start/end effects
    assert levels(sc)["Synced videos"] == "WARN"
    sc = check_session(make_session(tmp_path / "b", missed={0: 6}, sync=False))
    assert levels(sc)["Camera 1"] == "WARN"


def test_an_interrupted_session_fails_and_one_in_progress_does_not(tmp_path):
    s = make_session(tmp_path, ended=False)
    sc = check_session(s)
    assert sc.ended == "interrupted" and levels(sc)["Ended"] == "FAIL"

    # The heartbeat's own time decides (as in the app), not the file's.
    hb = s / "recording_heartbeat"
    hb.write_text(datetime.now(timezone.utc).isoformat(timespec="milliseconds"))
    sc = check_session(s, now=time.time())
    assert sc.ended == "recording" and levels(sc)["Ended"] == "INFO"
    hb.write_text((datetime.now(timezone.utc) - timedelta(minutes=10)).isoformat())
    assert check_session(s).ended == "interrupted"


def test_interview_mode_has_no_trigger_checks(tmp_path):
    sc = check_session(make_session(tmp_path, cams=(2,), mode="interview"))
    assert sc.mode == "interview"
    assert "Trigger log" not in levels(sc) and "Synced videos" not in levels(sc)
    assert levels(sc)["Camera 3"] == "INFO"  # free-running: no missed-frame count
    assert sc.result == "PASS"


def test_a_session_from_before_end_tracking_is_unknown_not_crashed(tmp_path):
    s = make_session(tmp_path)
    meta = json.loads((s / "session_meta.json").read_text())
    del meta["session_end"]
    (s / "session_meta.json").write_text(json.dumps(meta))
    sc = check_session(s)
    assert sc.ended == "unknown" and levels(sc)["Ended"] == "INFO"


def test_sync_repair_on_arrival_times_is_not_counted_as_missed_triggers(tmp_path):
    s = make_session(tmp_path, missed={1: 50})
    doc = json.loads((s / "synced" / "sync_repair.json").read_text())
    doc["alignment"] = "arrival_time"
    (s / "synced" / "sync_repair.json").write_text(json.dumps(doc))
    sc = check_session(s)
    assert sc.cameras[1].missed_from == "ticks"  # from the tick log instead
    assert sc.cameras[1].missed == 50


def test_odd_files_fail_that_session_only(tmp_path):
    s = make_session(tmp_path)
    (s / "video" / "action_group.json").write_text(json.dumps({"cameras": ["one"], "margin": "x"}))
    doc = json.loads((s / "synced" / "sync_repair.json").read_text())
    doc["cameras"][0]["missing_frame_count"] = None
    doc["cameras"].append({"index": "two"})
    (s / "synced" / "sync_repair.json").write_text(json.dumps(doc))
    (s / "video" / "timestamps_cam1.csv").write_bytes(b"\xff\xfe\x00bad")
    sc = check_session(s)  # no exception
    assert sc.margin is None


def test_unequal_synced_videos_fail(tmp_path):
    s = make_session(tmp_path)
    doc = json.loads((s / "synced" / "sync_repair.json").read_text())
    doc["cameras"][1]["output_frame_count"] -= 3
    (s / "synced" / "sync_repair.json").write_text(json.dumps(doc))
    assert levels(check_session(s))["Synced videos"] == "FAIL"


def test_missing_exposure_and_an_audio_stall_warn(tmp_path):
    sc = check_session(make_session(tmp_path, exposure=False, audio_stall=True))
    assert levels(sc)["Exposure column"] == "WARN"
    assert levels(sc)["Audio (audio.wav)"] == "WARN"


def test_not_a_session(tmp_path):
    sc = check_session(tmp_path)
    assert sc.result == "FAIL"


def test_find_sessions_newest_last_and_latest(tmp_path):
    make_session(tmp_path, "b", start="2026-10-14T11:00:00.000Z")
    make_session(tmp_path, "a", start="2026-10-14T12:00:00.000Z")
    make_session(tmp_path, "c", start="2026-10-14T10:00:00.000Z")
    make_session(tmp_path, "d.partial")
    assert [p.name for p in find_sessions([tmp_path])] == ["c", "b", "a"]
    assert [p.name for p in find_sessions([tmp_path], latest=2)] == ["b", "a"]
    assert [p.name for p in find_sessions([tmp_path / "b"])] == ["b"]


def test_cli_compares_sessions_and_sets_the_exit_code(tmp_path, capsys):
    make_session(tmp_path, "r85", margin=0.85, start="2026-10-14T10:00:00.000Z")
    make_session(tmp_path, "r95", margin=0.95, missed={0: 40}, start="2026-10-14T10:05:00.000Z")
    out_json = tmp_path / "report.json"
    code = run_session_check.main([str(tmp_path), "--json", str(out_json)])
    out = capsys.readouterr().out
    assert code == 1  # r95 fails
    table = out.split("Missed frames per camera:")[1]
    assert "r85" in table and "85%" in table and "95%" in table
    assert "40" in table.split("r95")[1]
    doc = json.loads(out_json.read_text())
    assert [d["result"] for d in doc] == ["PASS", "FAIL"]
    assert doc[1]["cameras"][0]["missed"] == 40

    assert run_session_check.main([str(tmp_path / "r85")]) == 0
    assert run_session_check.main([str(tmp_path / "nothing")]) == 2
