"""
Reads a recorded session's own files and judges it the way the room-11 test
recordings are judged: did it end cleanly, did every camera keep up with the
trigger, at what rate, and are the files each recording should have there.

Only the session's files are read (session_meta.json, video/, audio/,
synced/sync_repair.json); nothing is changed. Standard library only.

Missed frames per camera come from synced/sync_repair.json when Frame Sync
Repair has run (it places every frame on its trigger tick, so it knows
exactly which ticks have no frame); otherwise from the number of trigger
ticks fired against the frames the camera recorded, which can be off by a
tick or two at the start and end of a recording.
"""

from __future__ import annotations

import csv
import json
import statistics
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

#: Missed frames at or below this count as none: the first and last ticks of
#: a recording can fall outside a camera's frames without any loss.
END_TICKS = 2
#: A share of missed frames above this fails the session.
FAIL_SHARE = 0.02
#: A heartbeat younger than this (s) means the session is being recorded now:
#: kHeartbeatStaleSec in src/session/session_end.hpp, which the app uses.
HEARTBEAT_FRESH_S = 20


@dataclass
class Item:
    level: str  # PASS, WARN, FAIL, INFO
    label: str
    detail: str


@dataclass
class CameraResult:
    index: int  # 0-based, as in the file names
    frames: int | None = None
    configured_fps: float | None = None
    achieved_fps: float | None = None
    missed: int | None = None  # frames missing against the trigger ticks
    missed_from: str = ""  # "sync repair" or "ticks"
    pixel_format: str | None = None
    pixel_format_requested: str | None = None
    has_exposure: bool = False
    slots: int | None = None  # trigger ticks it should have answered (sync repair)

    @property
    def label(self) -> str:
        return f"Camera {self.index + 1}"


@dataclass
class SessionCheck:
    path: Path
    name: str
    mode: str = "?"
    ended: str = "?"  # clean, recording, interrupted
    duration_s: float | None = None
    margin: float | None = None
    tick_rate: float | None = None
    ticks: int | None = None
    cameras: list[CameraResult] = field(default_factory=list)
    items: list[Item] = field(default_factory=list)

    @property
    def result(self) -> str:
        levels = {i.level for i in self.items}
        return "FAIL" if "FAIL" in levels else "WARN" if "WARN" in levels else "PASS"

    def add(self, level: str, label: str, detail: str) -> None:
        self.items.append(Item(level, label, detail))


# ── Readers ───────────────────────────────────────────────────────────────────


def _read_json(path: Path) -> dict | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def _read_csv(path: Path) -> tuple[list[str], list[list[str]]]:
    try:
        with path.open(newline="", encoding="utf-8") as f:
            rows = list(csv.reader(f))
    except (OSError, UnicodeDecodeError, csv.Error):
        return [], []
    if not rows:
        return [], []
    return [h.strip() for h in rows[0]], [r for r in rows[1:] if r]


def _ticks(video: Path) -> tuple[int, float | None, float | None]:
    """(ticks fired, tick rate, span s) from action_ticks.csv."""
    header, rows = _read_csv(video / "action_ticks.csv")
    if "elapsed_ns" not in header:
        return 0, None, None
    t_col = header.index("elapsed_ns")
    f_col = header.index("fired") if "fired" in header else None
    times = []
    for r in rows:
        try:
            if f_col is None or int(r[f_col]) > 0:
                times.append(int(r[t_col]))
        except (ValueError, IndexError):
            continue
    if len(times) < 2:
        return len(times), None, None
    span = (times[-1] - times[0]) / 1e9
    return len(times), ((len(times) - 1) / span if span > 0 else None), span


# ── The check ─────────────────────────────────────────────────────────────────


def _int(v) -> int | None:
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def _num(v) -> float | None:
    return float(v) if isinstance(v, int | float) and not isinstance(v, bool) else None


def _heartbeat_utc(path: Path) -> float | None:
    """The time written inside the heartbeat file (the app reads it the same
    way: NTFS updates a file's modification time lazily), as a timestamp."""
    try:
        text = (path / "recording_heartbeat").read_text(encoding="utf-8").strip()
        return datetime.fromisoformat(text.replace("Z", "+00:00")).timestamp()
    except (OSError, ValueError):
        return None


def check_session(path: Path, now: float | None = None) -> SessionCheck:
    """Judges one session folder. A file it cannot make sense of fails this
    session only, with the reason."""
    try:
        return _check_session(Path(path), now)
    except Exception as exc:  # noqa: BLE001 - one odd session must not stop the rest
        sc = SessionCheck(path=Path(path), name=Path(path).name)
        sc.add("FAIL", "Session", f"could not be checked: {type(exc).__name__}: {exc}")
        return sc


def _check_session(path: Path, now: float | None) -> SessionCheck:
    sc = SessionCheck(path=path, name=path.name)
    meta = _read_json(path / "session_meta.json")
    if meta is None:
        sc.add("FAIL", "Session", "no readable session_meta.json: not a session folder")
        return sc

    rec = meta.get("recording") or {}
    sc.mode = rec.get("mode", "room")
    video_on = rec.get("video_enabled", True)

    # How it ended. No session_end key at all: recorded before MOSAIC kept
    # track (the app calls that unknown, not interrupted). A null one: it
    # never finished, unless its heartbeat is recent.
    end = meta.get("session_end")
    if "session_end" not in meta:
        sc.ended = "unknown"
        sc.add("INFO", "Ended", "unknown: recorded before MOSAIC tracked how sessions end")
    elif isinstance(end, dict):
        sc.ended = "clean"
        sc.duration_s = (_num(end.get("duration_ms")) or 0) / 1000.0 or None
        sc.add("PASS", "Ended", f"cleanly, after {sc.duration_s or 0:.1f} s")
    else:
        beat = _heartbeat_utc(path)
        age = (now if now is not None else time.time()) - beat if beat is not None else None
        if age is not None and age < HEARTBEAT_FRESH_S:
            sc.ended = "recording"
            sc.add("INFO", "Ended", "not yet: it is being recorded now")
        else:
            sc.ended = "interrupted"
            sc.add("FAIL", "Ended", "never finished: MOSAIC crashed, was ended or lost power")

    warnings = meta.get("preflight_warnings") or []
    if warnings:
        sc.add("INFO", "Recorded anyway", "; ".join(str(w) for w in warnings))

    video = path / "video"
    group = _read_json(video / "action_group.json")
    sc.ticks, sc.tick_rate, _span = _ticks(video)
    triggered = set()
    if group is not None:
        triggered = {i for i in (_int(c) for c in group.get("cameras") or []) if i is not None}
        sc.margin = _num(group.get("margin"))
    sync = _read_json(path / "synced" / "sync_repair.json")
    # Sync repair's missing frames are missed triggers only when it lined the
    # cameras up on the trigger ticks; aligned on arrival time, they are
    # frames repeated to match the fastest camera.
    on_ticks = str((sync or {}).get("alignment", "")).startswith("trigger_ticks")
    sync_cams = {}
    for entry in (sync or {}).get("cameras") or []:
        idx = _int(entry.get("index")) if isinstance(entry, dict) else None
        if idx is not None:
            sync_cams[idx] = entry

    for cam in meta.get("cameras", []):
        try:
            i = int(cam["index"])
        except (KeyError, TypeError, ValueError):
            continue
        ts = video / f"timestamps_cam{i}.csv"
        if not ts.exists() and not (video / f"video_{i}.mp4").exists():
            continue  # configured but not recorded (interview mode, or off)
        c = CameraResult(index=i)
        header, rows = _read_csv(ts)
        c.frames = _int(cam.get("frames_recorded"))
        if c.frames is None and header:
            c.frames = len(rows)
        c.has_exposure = "exposure_us" in header
        c.configured_fps = _num(cam.get("fps"))
        c.achieved_fps = _num(cam.get("achieved_fps"))
        c.pixel_format = cam.get("pixel_format")
        c.pixel_format_requested = cam.get("pixel_format_requested")
        s = sync_cams.get(i)
        missing = _int(s.get("missing_frame_count")) if s is not None else None
        if (
            on_ticks
            and i in triggered
            and s is not None
            and not s.get("skipped")
            and missing is not None
        ):
            c.missed, c.missed_from = missing, "sync repair"
            c.slots = _int(s.get("output_frame_count"))
        elif i in triggered and sc.ticks and c.frames is not None:
            c.missed, c.missed_from = max(0, sc.ticks - c.frames), "ticks"
        sc.cameras.append(c)
        _judge_camera(sc, c, has_group=group is not None)

    if video_on and not sc.cameras:
        sc.add("FAIL", "Cameras", "no camera recorded anything")

    _check_files(sc, path, meta, group, sync)
    return sc


def _judge_camera(sc: SessionCheck, c: CameraResult, has_group: bool = True) -> None:
    parts = []
    if c.frames is not None:
        parts.append(f"{c.frames} frames")
    if c.achieved_fps is not None:
        parts.append(f"{c.achieved_fps:.2f} fps")
    if c.pixel_format:
        fmt = c.pixel_format
        if c.pixel_format_requested and c.pixel_format_requested != c.pixel_format:
            fmt += f" (asked {c.pixel_format_requested})"
        parts.append(fmt)
    if c.missed is None:
        why = (
            "not in the trigger group (free-running)"
            if has_group
            else "no trigger log in this session (recorded before it existed, or not triggered)"
        )
        sc.add("INFO", c.label, ", ".join(parts) + f"; {why}, so no missed-frame count")
        return
    # Out of the trigger ticks it should have answered: sync repair's output
    # length (lead-in and tail trimmed) when known.
    total = c.slots if c.slots else (c.frames or 0) + c.missed
    share = c.missed / total if total else 0.0
    missed = f"{c.missed} missed ({share:.1%}, from {c.missed_from})"
    detail = ", ".join(parts + [missed])
    allowance = END_TICKS if c.missed_from == "ticks" else 0
    if c.missed <= allowance:
        sc.add("PASS", c.label, detail)
    elif share <= FAIL_SHARE:
        sc.add("WARN", c.label, detail)
    else:
        sc.add("FAIL", c.label, detail)


def _check_files(sc: SessionCheck, path: Path, meta: dict, group: dict | None, sync) -> None:
    if sc.mode == "room":
        if group is None:
            sc.add("WARN", "Trigger log", "video/action_group.json is missing")
        elif sc.ticks:
            rate = f"{sc.tick_rate:.2f} ticks/s" if sc.tick_rate else "rate unknown"
            margin = f", trigger rate {sc.margin:.0%}" if sc.margin is not None else ""
            sc.add("PASS", "Trigger log", f"{sc.ticks} ticks, {rate}{margin}")
        else:
            sc.add("WARN", "Trigger log", "video/action_ticks.csv is missing or empty")
        if len(sc.cameras) >= 2:
            if sync is None:
                sc.add(
                    "WARN",
                    "Synced videos",
                    "synced/sync_repair.json is missing (Frame Sync Repair did not run)",
                )
            else:
                counts = {
                    c.get("output_frame_count")
                    for c in sync.get("cameras", [])
                    if not c.get("skipped")
                }
                if len(counts) == 1:
                    sc.add("PASS", "Synced videos", f"every camera {counts.pop()} frames long")
                else:
                    sc.add(
                        "FAIL",
                        "Synced videos",
                        f"different lengths: {sorted(c for c in counts if c is not None)}",
                    )
    if sc.cameras and not all(c.has_exposure for c in sc.cameras):
        missing = ", ".join(c.label for c in sc.cameras if not c.has_exposure)
        sc.add("WARN", "Exposure column", f"no exposure_us in the timestamps of {missing}")
    elif sc.cameras:
        sc.add("PASS", "Exposure column", "in every camera's timestamps")
    # Only when this session's MOSAIC wrote it at all (older ones did not).
    has_any = any(c.achieved_fps is not None for c in sc.cameras)
    if sc.ended == "clean" and has_any and any(c.achieved_fps is None for c in sc.cameras):
        sc.add("WARN", "Achieved fps", "not written into session_meta.json for every camera")

    # Audio: one timing file per WAV, its rows arriving steadily.
    audio = path / "audio"
    if (meta.get("recording") or {}).get("audio_enabled", False) and audio.is_dir():
        for wav in sorted(audio.glob("*.wav")):
            timing = wav.with_suffix(".timing.csv")
            header, rows = _read_csv(timing)
            if "elapsed_ns" not in header:
                sc.add("WARN", f"Audio ({wav.name})", "no timing file beside it")
                continue
            col = header.index("elapsed_ns")
            try:
                t = [int(r[col]) for r in rows]
            except (ValueError, IndexError):
                sc.add("WARN", f"Audio ({wav.name})", "its timing file is unreadable")
                continue
            steps = [b - a for a, b in zip(t, t[1:], strict=False)]
            if len(steps) < 2:
                sc.add("WARN", f"Audio ({wav.name})", "its timing file has too few rows")
                continue
            med = statistics.median(steps)
            worst = max(steps)
            span = (t[-1] - t[0]) / 1e9
            detail = f"{len(t)} timing rows over {span:.1f} s, longest gap {worst / 1e6:.0f} ms"
            if med > 0 and worst > 10 * med and worst > 200e6:
                sc.add("WARN", f"Audio ({wav.name})", detail + " (an audio stall)")
            else:
                sc.add("PASS", f"Audio ({wav.name})", detail)


def find_sessions(paths: list[Path], latest: int | None = None) -> list[Path]:
    """Session folders among `paths`: each path is a session, or a folder of
    sessions (newest last). `latest` keeps the newest N of the latter."""
    out: list[Path] = []
    for p in paths:
        p = Path(p)
        if (p / "session_meta.json").exists():
            out.append(p)
            continue
        found = (
            [d for d in p.iterdir() if d.is_dir() and (d / "session_meta.json").exists()]
            if p.is_dir()
            else []
        )
        found = [d for d in found if not d.name.endswith((".partial", ".removing"))]
        found.sort(key=_start_key)
        if latest:
            found = found[-latest:]
        out.extend(found)
    return out


def _start_key(d: Path) -> str:
    meta = _read_json(d / "session_meta.json") or {}
    return str(meta.get("session_start_utc") or d.name)
