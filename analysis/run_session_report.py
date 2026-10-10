"""
MOSAIC session report: everything the analysis plugins found about a
recording, in one page and one row of numbers. See :mod:`report`.

    python analysis/run_session_report.py --session /path/to/session
    python analysis/run_session_report.py --sessions-root /path/to/recordings

``--session`` writes, in ``<session>/report/``:

* ``session_report.html``: one self-contained page (open it in any browser;
  print it to PDF to share).
* ``session_summary.csv`` and ``session_summary.json``: the session's
  numbers as one flat row, for statistics.

``--sessions-root`` finds every session below a folder (each folder with a
``session_meta.json``), writes each one's report, and combines their rows
into ``<root>/sessions_summary.csv``.

Nothing is analysed here: the report reads what the plugins already wrote,
so run the analyses first. Analyses not run yet are listed in the report.
"""

from __future__ import annotations

import argparse
import json
import sys
import traceback
from pathlib import Path

TAG = "[run_session_report]"


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="MOSAIC session report")
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument("--session", metavar="DIR", help="One recorded session")
    group.add_argument(
        "--sessions-root",
        metavar="DIR",
        nargs="+",
        help="Every session below these folders (the combined CSV goes in the first)",
    )
    p.add_argument(
        "--summary-only", action="store_true", help="Write the summary rows but not the HTML pages"
    )
    return p.parse_args(argv)


def log(msg: str) -> None:
    print(f"{TAG} {msg}", flush=True)


def report_session(session: Path, html: bool = True) -> dict:
    """Write one session's report files; return its summary row."""
    from report.collect import collect
    from report.render import render
    from report.summary import session_row, write_rows

    data = collect(session, for_summary_only=not html)
    row = session_row(data)
    out = session / "report"
    out.mkdir(exist_ok=True)
    write_rows(out / "session_summary.csv", [row])
    (out / "session_summary.json").write_text(json.dumps(row, indent=1), encoding="utf-8")
    if html:
        page = out / "session_report.html"
        page.write_text(render(data), encoding="utf-8")
        found = sum(len(c.results) for c in data.cameras) + len(data.session_results)
        log(f"{session.name}: {found} analysis result(s) -> {page}")
    return row


def find_sessions(root: Path) -> list[Path]:
    """Every session folder below ``root`` (a report folder's own copies
    are skipped; only folders below the root are looked at, not its name)."""
    root = Path(root)
    return sorted(
        p.parent
        for p in root.rglob("session_meta.json")
        if "report" not in p.relative_to(root).parts
    )


def main(argv=None) -> int:
    from report.summary import write_rows

    # A piped stdout on Windows is cp1252; session names may not be.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    args = parse_args(argv)
    if args.session:
        session = Path(args.session)
        if not (session / "session_meta.json").exists():
            print(f"{TAG} Not a session folder (no session_meta.json): {session}", file=sys.stderr)
            return 1
        report_session(session, html=not args.summary_only)
        return 0

    roots = [Path(r) for r in args.sessions_root]
    root = roots[0]
    sessions = sorted({s for r in roots for s in find_sessions(r)})
    if not sessions:
        print(f"{TAG} No sessions below {', '.join(map(str, roots))}", file=sys.stderr)
        return 1
    rows = []
    for pos, session in enumerate(sessions, start=1):
        print(f"  {100.0 * pos / len(sessions):5.1f}%  ({pos}/{len(sessions)})", flush=True)
        try:
            rows.append(report_session(session, html=not args.summary_only))
        except Exception:  # one unreadable session must not stop the others
            traceback.print_exc()
            log(f"{session.name}: failed (see the error above)")
    out = root / "sessions_summary.csv"
    try:
        write_rows(out, rows)
    except PermissionError:
        print(f"{TAG} Cannot write {out}: close it (Excel?) and run again.", file=sys.stderr)
        return 1
    log(f"{len(rows)} session(s) -> {out}")
    return 0 if rows else 1


if __name__ == "__main__":
    sys.exit(main())
