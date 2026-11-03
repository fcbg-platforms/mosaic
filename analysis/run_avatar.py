"""
MOSAIC 3D Avatar: a realistic body (Anny, a body model for all ages) fitted
to each person of the session's 3D Pose result, for the 3D views.

    python analysis/run_avatar.py --session DIR [--person N ...] [--cpu]

Needs 3D Pose (``skeleton3d.json``). With 3D hands (``hands/hands3d.frames.json``,
from Hands on two or more calibrated cameras), the fingers too. Writes, in
``<session>/avatar/``:

* ``avatar.json``: per person its body shape (Anny's phenotype: gender, age,
  muscle, weight, height, proportions), how well the body fits the 3D Pose
  joints (median and 90th percentile distance, over all and per joint, mm),
  per hand with 3D hands how many frames and how well its fingers fit them,
  and where its file is and what is in it; the people that could not be
  fitted; the bones' names, in the files' order; and ``to_room``, from the
  fit's frame to the room's;
* ``person_N.bin``: the body mesh at rest, its skinning, and every tick's
  bone transforms (layout in avatar/io.py).

With a GPU, about two to three minutes per minute of recording per person;
several times longer without. ``--person`` fits some people again and keeps
the others' results. See docs/math/avatar.rst.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import traceback
from pathlib import Path

import numpy as np
from avatar import io
from avatar.hands import assign, load_hands
from avatar.people import load_people, up_frame, usable, valid_bits

TAG = "[run_avatar]"
KEYPOINTS = [
    "nose", "left_eye", "right_eye", "left_ear", "right_ear", "left_shoulder",
    "right_shoulder", "left_elbow", "right_elbow", "left_wrist", "right_wrist", "left_hip",
    "right_hip", "left_knee", "right_knee", "left_ankle", "right_ankle",
]  # fmt: skip


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="MOSAIC 3D Avatar (Anny)")
    p.add_argument("--session", required=True, metavar="DIR")
    p.add_argument("--person", type=int, nargs="+", help="Only these 3D Pose people")
    p.add_argument("--cpu", action="store_true", help="Fit on the CPU even with a GPU")
    return p.parse_args(argv)


def log(msg: str) -> None:
    print(f"{TAG} {msg}", flush=True)


def quality(fit: dict) -> dict:
    """How far the fitted body's keypoints are from 3D Pose's, over the
    ticks each was measured (mm): the median and the 90th percentile (the
    median alone hides a body that is far off now and then), over all and
    per joint."""
    err = fit["error_m"] * 1000.0

    def stat(e, q):
        e = e[np.isfinite(e)]
        return round(float(np.percentile(e, q)), 1) if len(e) else None

    return {
        "median_error_mm": stat(err.ravel(), 50),
        "p90_error_mm": stat(err.ravel(), 90),
        "per_joint_median_error_mm": {n: stat(err[:, k], 50) for k, n in enumerate(KEYPOINTS)},
        "per_joint_p90_error_mm": {n: stat(err[:, k], 90) for k, n in enumerate(KEYPOINTS)},
    }


def hand_quality(hands: dict, rate_hz: float) -> dict:
    """Per hand fitted: its 3D hand frames, the seconds of ticks it has
    fingers for, and how far its fitted points are from 3D Hands' (taken
    from the knuckles' middle, mm: the median and the 90th percentile)."""
    out = {}
    for side in ("left", "right"):
        if side not in hands:
            continue
        h = hands[side]
        e = h["error_m"][np.isfinite(h["error_m"])] * 1000.0
        out[side] = {
            "frames": int(h["frames"]),
            "seconds": round(float(h["on"].sum()) / rate_hz, 1),
            "median_error_mm": round(float(np.median(e)), 1) if len(e) else None,
            "p90_error_mm": round(float(np.percentile(e, 90)), 1) if len(e) else None,
        }
    return out


def main(argv=None, fitter=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    args = parse_args(argv)
    session = Path(args.session)
    src = session / "skeleton3d.json"
    if not src.exists():
        print(f"{TAG} No 3D Pose result ({src.name}): run 3D Pose first.", file=sys.stderr)
        return 1
    found = load_people(json.loads(src.read_text(encoding="utf-8")))
    # Up from everyone, so that fitting some people again keeps the frame.
    M = up_frame(np.concatenate([p["keypoints"] for p in found.values()])) if found else None
    people = {k: v for k, v in found.items() if not args.person or k in set(args.person)}
    too_few = [
        k for k, v in people.items() if not usable(np.isfinite(v["keypoints"]).all(-1)).any()
    ]
    for k in too_few:
        log(f"person {k}: too few joints (no tick with the hips, shoulders and 8 joints)")
    people = {k: v for k, v in people.items() if k not in too_few}
    if not people:
        print(f"{TAG} Nobody to fit in {src.name}.", file=sys.stderr)
        return 1
    hands = assign(load_hands(session / "hands" / "hands3d.frames.json"), found)
    for pid, sides in sorted(hands.items()):
        if pid in people:
            log(f"person {pid}: 3D hands, " + ", ".join(sorted(sides)))
    if fitter is None:
        from avatar.fit import AnnyFitter

        log("Loading Anny (the first time builds its cache: a few minutes)")
        fitter = AnnyFitter("cpu" if args.cpu else None, log=log)
    out_dir = session / "avatar"
    out_dir.mkdir(exist_ok=True)
    # With --person, the other people's earlier results are kept; a full run
    # starts over, removing every earlier person's file.
    earlier = {}
    old = out_dir / "avatar.json"
    if args.person and old.exists():
        try:
            earlier = {e["person_id"]: e for e in json.loads(old.read_text())["people"]}
        except (OSError, ValueError, KeyError):
            earlier = {}
    if not args.person:
        for f in out_dir.glob("person_*.bin"):
            f.unlink()
    entries, failed = [], list(too_few)
    for pos, (pid, person) in enumerate(sorted(people.items()), start=1):
        n = len(person["ticks"])
        earlier.pop(pid, None)
        log(f"Step {pos}/{len(people)}: person {pid}, {n} ticks")
        t0 = time.perf_counter()
        try:
            kp_zup = np.einsum("ij,nkj->nki", M, person["keypoints"])
            fit = fitter.fit(kp_zup, person["ticks"], log=lambda m: print(m, flush=True))
            if pid in hands:
                fit["hands"] = fitter.fit_hands(
                    fit,
                    {
                        side: {
                            "time_ms": h["time_ms"],
                            "points": np.einsum("ij,nkj->nki", M, h["points"]),
                        }
                        for side, h in hands[pid].items()
                    },
                    person["time_ms"],
                    log=lambda m: print(m, flush=True),
                )
            arrays = fitter.export(fit)
            arrays["time_ms"] = person["time_ms"]
            arrays["valid"] = valid_bits(person["keypoints"])
            path = out_dir / f"person_{pid}.bin"
            counts = io.write_person(path, arrays)
        except Exception:  # one person's failure must not stop the others
            traceback.print_exc()
            log(f"person {pid}: failed (see the error above)")
            failed.append(pid)
            continue
        q = quality(fit)
        if fit.get("hands"):
            dt = np.diff(person["time_ms"])
            rate = 1000.0 / float(np.median(dt)) if len(dt) else 25.0
            q["hands"] = hand_quality(fit["hands"], rate)
        entries.append(
            {
                "person_id": pid,
                "file": f"avatar/{path.name}",
                **counts,
                "phenotype": {k: round(v, 3) for k, v in fit["phenotype"].items()},
                "hip_offset_mm": np.round(fit["hip_offset_m"].astype(float) * 1000.0, 1).tolist(),
                "seconds": round(float(person["time_ms"][-1] - person["time_ms"][0]) / 1000.0, 1),
                **q,
            }
        )
        log(
            f"person {pid}: distance to the 3D Pose joints median {q['median_error_mm']} mm, "
            f"90th percentile {q['p90_error_mm']} mm, {time.perf_counter() - t0:.0f} s -> {path}"
        )
        for side, h in q.get("hands", {}).items():
            log(
                f"person {pid}: {side} fingers on {h['frames']} 3D hand frame(s), "
                f"{h['seconds']} s; distance median {h['median_error_mm']} mm, "
                f"90th percentile {h['p90_error_mm']} mm"
            )
    result = {
        "schema": io.SCHEMA,
        "source": src.name,
        "body_model": "anny",
        "keypoint_names": KEYPOINTS,
        # The bones of every person's file, in its order (the same body model).
        "bone_labels": list(fitter.labels),
        # The fit's frame (metres, z up) to the room's (mm): room = to_room @ p.
        "to_room": (M.T * 1000.0).round(6).tolist(),
        "people": sorted([*earlier.values(), *entries], key=lambda e: e["person_id"]),
        "failed": sorted(failed),
    }
    path = out_dir / "avatar.json"
    path.write_text(json.dumps(result, indent=1), encoding="utf-8")
    log(f"{len(result['people'])} avatar(s) -> {path}")
    return 1 if not entries else 0


if __name__ == "__main__":
    sys.exit(main())
