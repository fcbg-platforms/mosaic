"""
MOSAIC Social Interaction: how two people's behaviour is coupled, from the
analyses already run on the two cameras that face them.

    python analysis/run_social.py --session DIR [--camera-a 1] [--camera-b 4]

Camera indices count from 0 (index 1 is "Camera 2" in the app). Reads:

* Face Dynamics: head movement, smiles and nods of each person;
* Pose: body movement of the main person each camera sees;
* Remote Heart Rate: each person's heart rate.

Writes ``<session>/social/social.json`` and ``social/synchrony.csv``:

* movement synchrony (head and body) and heart-rate synchrony: windowed
  cross-correlation against surrogate pairs (:mod:`social.synchrony`);
* smile and nod mimicry, both ways, against chance (:mod:`social.mimicry`).

Nothing is analysed from the video here; run those analyses first. See
docs/math/social_interaction.rst for the method.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

import numpy as np
from social import signals
from social.mimicry import mimicry
from social.synchrony import synchrony

TAG = "[run_social]"
#: Movement: 10 samples per second, 5 s windows every 0.5 s, lags up to 3 s.
MOVE = {"fs": 10.0, "window_s": 5.0, "step_s": 0.5, "max_lag_s": 3.0}
#: Heart rate changes slowly: 1 per second, 30 s windows every 5 s, lags up to 10 s.
HEART = {"fs": 1.0, "window_s": 30.0, "step_s": 5.0, "max_lag_s": 10.0}
MIMICRY_WITHIN_S = 3.0


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="MOSAIC social interaction")
    p.add_argument("--session", required=True, metavar="DIR")
    p.add_argument("--camera-a", type=int, default=1, help="Camera facing person A (0-based)")
    p.add_argument("--camera-b", type=int, default=4, help="Camera facing person B (0-based)")
    return p.parse_args(argv)


def log(msg: str) -> None:
    print(f"{TAG} {msg}", flush=True)


def video_start_ms(session: Path, cam: int) -> float | None:
    path = session / "video" / f"timestamps_cam{cam}.csv"
    try:
        with path.open(newline="") as f:
            for row in csv.DictReader(f):
                try:
                    return int(row["elapsed_ns"]) / 1e6
                except (KeyError, TypeError, ValueError):
                    continue
    except OSError:
        return None
    return None


#: A camera whose signal is present less than this share of the time likely
#: does not face the person.
MIN_COVERAGE = 0.2


def _coverage_note(what: str, name: str, values) -> str | None:
    v = np.asarray(values, float)
    share = float(np.isfinite(v).mean()) if v.size else 0.0
    if share >= MIN_COVERAGE:
        return None
    return (
        f"{what}: {name} measured it only {100 * share:.0f}% of the time; does that camera "
        "face the person? Choose the camera that does."
    )


def min_seconds(params: dict) -> float:
    """The shortest stretch both people must cover: surrogate shifts of at
    least one window plus both lags each way, so chance can be measured."""
    return 2.0 * (params["window_s"] + 2.0 * params["max_lag_s"]) + 1.0


def _too_short(what: str, ta, tb, params: dict) -> str | None:
    ta, tb = np.asarray(ta, float), np.asarray(tb, float)
    if ta.size == 0 or tb.size == 0:
        return None
    both_s = (min(ta.max(), tb.max()) - max(ta.min(), tb.min())) / 1000.0
    need = min_seconds(params)
    if both_s >= need:
        return None
    return (
        f"{what}: the two cameras share only {max(0.0, both_s):.0f} s; at least {need:.0f} s "
        "are needed to compare with chance."
    )


def _pair_synchrony(ta, va, tb, vb, params: dict, max_gap_s: float = 0.0) -> dict | None:
    """Both signals on one grid over the stretch both cover, then WCC."""
    ta, tb = np.asarray(ta, float), np.asarray(tb, float)
    if ta.size == 0 or tb.size == 0:
        return None
    t0, t1 = max(ta.min(), tb.min()), min(ta.max(), tb.max())
    if not np.isfinite(t0) or not np.isfinite(t1) or t1 <= t0:
        return None
    fs = params["fs"]
    n = int((t1 - t0) * fs / 1000.0) + 1
    if n < min_seconds(params) * fs:
        return None
    x = signals.on_grid(ta, va, t0, n, fs, max_gap_s)
    y = signals.on_grid(tb, vb, t0, n, fs, max_gap_s)
    res = synchrony(x, y, fs, params["window_s"], params["step_s"], params["max_lag_s"])
    if res is not None:
        res["t0_ms"] = round(t0, 1)
    return res


def analyse(session: Path, cam_a: int, cam_b: int) -> dict:
    a_name, b_name = f"Camera {cam_a + 1}", f"Camera {cam_b + 1}"
    out = {
        "schema": "mosaic-social-v1",
        "cameras": {"a": cam_a, "b": cam_b},
        "video_start_ms": {
            "a": video_start_ms(session, cam_a),
            "b": video_start_ms(session, cam_b),
        },
        "synchrony": {},
        "mimicry": {},
        "notes": [],
    }

    def missing(what: str, analysis: str, have_a, have_b):
        cams = [n for n, h in ((a_name, have_a), (b_name, have_b)) if not h]
        out["notes"].append(f"{what}: run {analysis} on {' and '.join(cams)}.")

    fa, fb = signals.face_dynamics(session, cam_a), signals.face_dynamics(session, cam_b)
    if fa and fb:
        log("Head movement synchrony")
        r = _pair_synchrony(fa["t_ms"], fa["head_speed"], fb["t_ms"], fb["head_speed"], MOVE)
        if r:
            out["synchrony"]["head"] = r
        else:
            out["notes"].append(
                _too_short("Head movement", fa["t_ms"], fb["t_ms"], MOVE)
                or _coverage_note("Head movement", a_name, fa["head_speed"])
                or _coverage_note("Head movement", b_name, fb["head_speed"])
                or "Head movement: too little time with a face on both cameras."
            )
        span = (
            max(fa["t_ms"].min(), fb["t_ms"].min()) / 1000.0,
            min(fa["t_ms"].max(), fb["t_ms"].max()) / 1000.0,
        )
        for kind in ("smile", "nod"):
            ea = [t / 1000.0 for t in fa["onsets_ms"].get(kind, [])]
            eb = [t / 1000.0 for t in fb["onsets_ms"].get(kind, [])]
            out["mimicry"][kind] = {
                "a_to_b": mimicry(ea, eb, span, MIMICRY_WITHIN_S),
                "b_to_a": mimicry(eb, ea, span, MIMICRY_WITHIN_S),
            }
    else:
        missing("Head movement and mimicry", "Face Dynamics", fa, fb)

    log("Body movement synchrony")
    ba, bb = signals.body_movement(session, cam_a), signals.body_movement(session, cam_b)
    if ba and bb:
        r = _pair_synchrony(ba["t_ms"], ba["body_speed"], bb["t_ms"], bb["body_speed"], MOVE)
        if r:
            out["synchrony"]["body"] = r
        else:
            out["notes"].append(
                _too_short("Body movement", ba["t_ms"], bb["t_ms"], MOVE)
                or _coverage_note("Body movement", a_name, ba["body_speed"])
                or _coverage_note("Body movement", b_name, bb["body_speed"])
                or "Body movement: too little time with a person on both cameras."
            )
    else:
        missing("Body movement", "Pose", ba, bb)

    log("Heart-rate synchrony")
    ha, hb = signals.heart_rate(session, cam_a), signals.heart_rate(session, cam_b)
    if ha and hb:
        r = _pair_synchrony(ha["t_ms"], ha["bpm"], hb["t_ms"], hb["bpm"], HEART, max_gap_s=5.0)
        if r:
            out["synchrony"]["heart_rate"] = r
        else:
            out["notes"].append(
                _too_short("Heart rate", ha["t_ms"], hb["t_ms"], HEART)
                or "Heart rate: too little clear heart-rate signal on both cameras at once."
            )
    else:
        missing("Heart rate", "Remote Heart Rate", ha, hb)
    return out


def write(session: Path, res: dict) -> Path:
    folder = session / "social"
    folder.mkdir(exist_ok=True)
    path = folder / "social.json"
    path.write_text(json.dumps(res, indent=1), encoding="utf-8")
    with (folder / "synchrony.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["measure", "t_ms", "peak_r", "peak_lag_s"])
        for name, m in res["synchrony"].items():
            s = m["series"]
            for t, r, lag in zip(s["t_s"], s["peak_r"], s["peak_lag_s"], strict=False):
                w.writerow([name, round(m["t0_ms"] + 1000.0 * t, 1), r, lag])
    return path


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
    if (meta.get("recording") or {}).get("mode") == "interview":
        print(
            f"{TAG} This is an interview recording: only one person is on camera, so there is "
            "no second person to compare with. Use a room recording with both people on camera.",
            file=sys.stderr,
        )
        return 1
    if args.camera_a == args.camera_b:
        print(f"{TAG} Choose two different cameras for the two people.", file=sys.stderr)
        return 1
    for cam in (args.camera_a, args.camera_b):
        if not (session / "video" / f"video_{cam}.mp4").exists():
            print(f"{TAG} This session has no video for Camera {cam + 1}.", file=sys.stderr)
            return 1
    log(f"Person A: Camera {args.camera_a + 1}; person B: Camera {args.camera_b + 1}")
    res = analyse(session, args.camera_a, args.camera_b)
    if not res["synchrony"] and not res["mimicry"]:
        for note in res["notes"]:
            print(f"{TAG} {note}", file=sys.stderr)
        print(f"{TAG} Nothing could be compared (see the notes above).", file=sys.stderr)
        return 1
    path = write(session, res)
    for name, m in res["synchrony"].items():
        s = m["summary"]
        log(
            f"{name}: mean peak r {s['mean_peak_r']} vs surrogates {s['surrogate_mean_peak_r']} "
            f"(p = {s['p_value']})"
        )
    for note in res["notes"]:
        log(note)
    log(f"Social interaction -> {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
