"""
Turn a Cobertura coverage report into a Markdown summary: covered lines per
folder, then the least covered files. CI appends it to the run's summary page
($GITHUB_STEP_SUMMARY) for both the Python tests (coverage.py) and the C++
tests (OpenCppCoverage). Standard library only.

Usage::

    python scripts/coverage_summary.py REPORT.xml --title "C++" [--root src]

``--root`` is the folder the file paths are reported relative to: everything
up to and including it is dropped from each path, and the first folder below
it names the group. Without it, paths are used as written.
"""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import PurePosixPath

#: Files with fewer lines than this are left out of the "least covered" list.
MIN_LINES_TO_LIST = 20
#: How many of the least covered files to list.
WORST_FILES = 10


def relative_path(filename: str, root: str | None) -> str:
    """The report's file name with forward slashes, relative to ``root``."""
    path = filename.replace("\\", "/")
    if root:
        marker = "/" + root.strip("/") + "/"
        probe = "/" + path
        at = probe.lower().rfind(marker.lower())
        if at >= 0:
            path = probe[at + len(marker) :]
    return path.lstrip("/")


def read_lines(report: str, root: str | None) -> dict[str, dict[int, bool]]:
    """Per file: line number -> whether any test ran it. A file reported more
    than once (OpenCppCoverage lists it once per module) is merged."""
    files: dict[str, dict[int, bool]] = defaultdict(dict)
    for cls in ET.parse(report).getroot().iter("class"):
        name = relative_path(cls.get("filename", ""), root)
        lines = files[name]
        for line in cls.iter("line"):
            number = int(line.get("number", "0"))
            hit = int(line.get("hits", "0")) > 0
            lines[number] = lines.get(number, False) or hit
    return files


def group_of(path: str) -> str:
    parts = PurePosixPath(path).parts
    return parts[0] + "/" if len(parts) > 1 else "(top level)"


def percent(covered: int, total: int) -> str:
    return f"{100.0 * covered / total:.1f}%" if total else "n/a"


def summarise(files: dict[str, dict[int, bool]], title: str) -> str:
    groups: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    per_file = []
    for path, lines in files.items():
        total = len(lines)
        covered = sum(lines.values())
        if total == 0:
            continue
        g = groups[group_of(path)]
        g[0] += covered
        g[1] += total
        per_file.append((covered / total, total, covered, path))

    all_covered = sum(c for c, _ in groups.values())
    all_total = sum(t for _, t in groups.values())
    out = [f"### Coverage: {title}", ""]
    out.append(
        f"**{percent(all_covered, all_total)}** of {all_total:,} lines run by the tests "
        f"({len(per_file)} files)."
    )
    out += ["", "| Folder | Lines | Run by tests | Coverage |", "|---|---:|---:|---:|"]
    for name, (covered, total) in sorted(groups.items()):
        out.append(f"| `{name}` | {total:,} | {covered:,} | {percent(covered, total)} |")

    worst = sorted(f for f in per_file if f[1] >= MIN_LINES_TO_LIST)[:WORST_FILES]
    if worst:
        out += [
            "",
            f"Least covered files ({MIN_LINES_TO_LIST}+ lines):",
            "",
            "| File | Lines | Coverage |",
            "|---|---:|---:|",
        ]
        for _, total, covered, path in worst:
            out.append(f"| `{path}` | {total:,} | {percent(covered, total)} |")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("report", help="Cobertura XML report")
    parser.add_argument("--title", required=True)
    parser.add_argument("--root", default=None)
    args = parser.parse_args(argv)
    files = read_lines(args.report, args.root)
    if not files:
        print(f"### Coverage: {args.title}\n\nThe report lists no files.\n")
        return 0
    sys.stdout.write(summarise(files, args.title))
    return 0


if __name__ == "__main__":
    sys.exit(main())
