"""
MOSAIC audio-video check: claps heard against claps seen.

    python analysis/run_clap_check.py --session DIR [--camera N]

Clap three times, clearly, with uneven gaps (clap, 1 s, clap, 2 s, clap),
hands in view of the cameras; the start of a recording is enough. This
finds the claps in each microphone's audio and, in each camera with a Pose
result, the moments the hands met, pairs them, and
gives per camera how far the audio is placed from the video: the median of
audio time minus video time, its spread, and a verdict (ok within half a
frame, check within a frame, off beyond). Writes
``<session>/sync_check/clap_check.json``. See docs/math/clap_check.rst.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

import numpy as np
from sync_check import clap

TAG = "[run_clap_check]"
SCHEMA = "mosaic-clap-check-v1"
WRISTS = (9, 10)


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="MOSAIC audio-video clap check")
    p.add_argument("--session", required=True, metavar="DIR")
    p.add_argument("--camera", type=int, default=None, help="Only this camera index")
    return p.parse_args(argv)


def log(msg: str) -> None:
    print(f"{TAG} {msg}", flush=True)


def audio_clock(wav: Path, rate: int, meta: dict):
    """The audio's placement on the video clock, as every analysis uses it."""
    from conversation import timing as tm

    timing_csv = wav.with_suffix(".timing.csv")
    if timing_csv.exists():
        clock = tm.fit_timing(*tm.read_timing_csv(timing_csv), rate)
        if clock is not None:
            return clock
    start = meta.get("session_start_elapsed_ns")
    if start is None:
        return None
    return tm.AudioClock(float(start), 1e9 / rate, "session_start")


def heard(session: Path, meta: dict) -> list:
    """Per microphone: its name, how it is placed, and its claps (ms)."""
    from conversation.vad import read_wav

    out = []
    for wav in sorted((session / "audio").glob("*.wav")):
        x, rate = read_wav(wav)
        clock = audio_clock(wav, rate, meta)
        if clock is None:
            log(f"{wav.name}: cannot be placed on the video clock (no timing file, no start)")
            continue
        onsets = clap.onset_times(x, rate)
        ms = clock.sample_time_ns(onsets * rate) / 1e6
        out.append({"file": wav.name, "clock": clock.method, "claps_ms": ms})
    return out


def wrist_gap(pose_json: Path) -> tuple:
    """Per Pose frame: its time (ms) and the main person's wrist distance in
    body heights (NaN when a wrist was not seen)."""
    d = json.loads(pose_json.read_text(encoding="utf-8"))
    t, g = [], []
    for f in d.get("frames", []):
        ts = f.get("timestamp_ns")
        if not ts:  # None, or 0: Pose writes 0 for a frame without a time
            continue
        people = [p for p in f.get("subjects") or [] if p.get("bbox_xyxy")]
        gap = np.nan
        if people:
            p = max(
                people,
                key=lambda q: (q["bbox_xyxy"][2] - q["bbox_xyxy"][0])
                * (q["bbox_xyxy"][3] - q["bbox_xyxy"][1]),
            )
            kp = np.asarray(p.get("keypoints") or [], float).reshape(-1, 2)
            vis = np.asarray(p.get("visibilities") or [], float)
            height = max(1.0, p["bbox_xyxy"][3] - p["bbox_xyxy"][1])
            a, b = WRISTS
            if max(a, b) < min(len(kp), len(vis)) and vis[a] > 0.3 and vis[b] > 0.3:
                gap = float(np.linalg.norm(kp[a] - kp[b]) / height)
        t.append(ts / 1e6)
        g.append(gap)
    return np.asarray(t), np.asarray(g)


def camera_frame_ms(session: Path, cam: int, fallback: float) -> float:
    """The camera's own frame interval, ms, from its timestamps (Pose may
    have analysed only every Nth frame)."""
    path = session / "video" / f"timestamps_cam{cam}.csv"
    try:
        with path.open(newline="") as f:
            t = [int(r["elapsed_ns"]) for r in csv.DictReader(f) if r.get("elapsed_ns")]
    except (OSError, ValueError, KeyError):
        return fallback
    d = np.diff(np.asarray(t, dtype=np.float64)) / 1e6
    d = d[d > 0]
    return float(np.median(d)) if d.size else fallback


def check_camera(session: Path, cam: int, pose_json: Path, mics: list) -> dict:
    t, g = wrist_gap(pose_json)
    seen = clap.seen_claps(t, g)
    steps = np.diff(t)
    steps = steps[steps > 0]
    frame_ms = camera_frame_ms(session, cam, float(np.median(steps)) if steps.size else 40.0)
    res = {"camera": cam, "seen": len(seen), "frame_ms": round(frame_ms, 2), "microphones": []}
    for mic in mics:
        pairs = clap.match(mic["claps_ms"], seen)
        offs = [p[2] for p in pairs]
        med = float(np.median(offs)) if offs else None
        res["microphones"].append(
            {
                "file": mic["file"],
                "clock": mic["clock"],
                "matched": len(pairs),
                "median_offset_ms": None if med is None else round(med, 1),
                "spread_ms": None
                if not offs
                else round(float(np.median(np.abs(np.asarray(offs) - med))), 1),
                "verdict": clap.verdict(offs, frame_ms),
                "pairs": [
                    {"seen_ms": round(s, 1), "heard_ms": round(h, 1), "offset_ms": round(o, 1)}
                    for s, h, o in pairs
                ],
            }
        )
    return res


def main(argv=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    args = parse_args(argv)
    session = Path(args.session)
    try:
        meta = json.loads((session / "session_meta.json").read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        print(f"{TAG} Not a session folder (no session_meta.json): {session}", file=sys.stderr)
        return 1
    mics = heard(session, meta)
    if not mics:
        print(f"{TAG} No audio to check in {session / 'audio'}.", file=sys.stderr)
        return 1
    for m in mics:
        log(f"{m['file']}: {len(m['claps_ms'])} sharp sound(s), placed by {m['clock']}")
    cams = []
    for video in sorted((session / "video").glob("video_*.mp4")):
        cam = int(video.stem.split("_")[1])
        if args.camera is not None and cam != args.camera:
            continue
        poses = sorted(
            (session / "pose").glob(f"{video.stem}.*.pose.json"), key=lambda p: p.stat().st_mtime
        )
        if not poses:
            log(f"Camera {cam + 1}: no Pose result, so no hands to see: run Pose on it")
            continue
        r = check_camera(session, cam, poses[-1], mics)
        cams.append(r)
        for m in r["microphones"]:
            off = m["median_offset_ms"]
            log(
                f"Camera {cam + 1} / {m['file']}: {r['seen']} clap(s) seen, {m['matched']} matched"
                + ("" if off is None else f", audio {off:+.0f} ms from the video ({m['verdict']})")
            )
    if not cams:
        print(f"{TAG} No camera with a Pose result: run Pose first.", file=sys.stderr)
        return 1
    order = ["none", "ok", "check", "off"]
    worst = max((m["verdict"] for c in cams for m in c["microphones"]), key=order.index)
    out = session / "sync_check"
    out.mkdir(exist_ok=True)
    doc = {
        "schema": SCHEMA,
        "verdict": worst,
        "audio": [
            {"file": m["file"], "clock": m["clock"], "heard": len(m["claps_ms"])} for m in mics
        ],
        "cameras": cams,
    }
    path = out / "clap_check.json"
    path.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    if worst == "none":
        log("No clap was both heard and seen: clap clearly, hands in view, and run again.")
    log(f"Audio-video check ({worst}) -> {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
