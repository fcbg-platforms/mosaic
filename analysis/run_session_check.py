#!/usr/bin/env python3
"""
Check test recordings: did each end cleanly, did every camera keep up with
the trigger, at what rate, and are the files a recording should have there.

Usage::

    python analysis/run_session_check.py SESSION [SESSION ...]
    python analysis/run_session_check.py recordings/guest --latest 4
    python analysis/run_session_check.py SESSION --json report.json

Each argument is a session folder, or a folder of sessions (``--latest N``
keeps the newest N of those). With several sessions, a table compares them
side by side: the trigger-rate test in one view. Reads the sessions' files
only. The exit code is 1 when any session fails.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from session_check import SessionCheck, check_session, find_sessions  # noqa: E402

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def print_session(sc: SessionCheck) -> None:
    print(f"\n{sc.name}  [{sc.mode}]  {sc.result}")
    for it in sc.items:
        print(f"  {it.level:<4}  {it.label:<18} {it.detail}")


def comparison_table(checks: list[SessionCheck]) -> str:
    """One row per session: its trigger rate and each camera's missed frames."""
    cams = sorted({c.index for sc in checks for c in sc.cameras})
    head = ["Session", "Rate", "Ticks/s"] + [f"Cam {i + 1}" for i in cams] + ["Result"]
    rows = []
    for sc in checks:
        by = {c.index: c for c in sc.cameras}
        row = [
            sc.name,
            f"{sc.margin:.0%}" if sc.margin is not None else "-",
            f"{sc.tick_rate:.2f}" if sc.tick_rate else "-",
        ]
        for i in cams:
            c = by.get(i)
            row.append("-" if c is None or c.missed is None else str(c.missed))
        row.append(sc.result)
        rows.append(row)
    widths = [max(len(r[k]) for r in [head, *rows]) for k in range(len(head))]
    lines = [
        "  ".join(v.ljust(w) for v, w in zip(r, widths, strict=True)).rstrip()
        for r in [head, *rows]
    ]
    lines.insert(1, "  ".join("-" * w for w in widths))
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="+", type=Path, help="session folders, or folders of sessions")
    ap.add_argument(
        "--latest", type=int, default=None, help="only the newest N sessions of a folder"
    )
    ap.add_argument("--json", type=Path, default=None, help="also write the results to this file")
    args = ap.parse_args(argv)

    sessions = find_sessions(args.paths, args.latest)
    if not sessions:
        print(
            "No session folders found (a session folder holds session_meta.json).", file=sys.stderr
        )
        return 2
    checks = [check_session(p) for p in sessions]
    for sc in checks:
        print_session(sc)
    if len(checks) > 1:
        print("\nMissed frames per camera:\n")
        print(comparison_table(checks))

    if args.json:
        doc = [
            {
                "session": str(sc.path),
                "result": sc.result,
                "mode": sc.mode,
                "ended": sc.ended,
                "duration_s": sc.duration_s,
                "trigger_rate": sc.margin,
                "ticks": sc.ticks,
                "ticks_per_s": sc.tick_rate,
                "cameras": [
                    {
                        "camera": c.index + 1,
                        "frames": c.frames,
                        "achieved_fps": c.achieved_fps,
                        "missed": c.missed,
                        "missed_from": c.missed_from or None,
                    }
                    for c in sc.cameras
                ],
                "items": [
                    {"level": i.level, "check": i.label, "detail": i.detail} for i in sc.items
                ],
            }
            for sc in checks
        ]
        args.json.write_text(json.dumps(doc, indent=2), encoding="utf-8")
        print(f"\nWritten to {args.json}")
    return 1 if any(sc.result == "FAIL" for sc in checks) else 0


if __name__ == "__main__":
    sys.exit(main())
