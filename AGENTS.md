# AGENTS.md

This file guides AI coding agents (Claude Code, Copilot, Cursor, ...) working in this repository.
Humans may find it useful too; `CONTRIBUTING.md` is the general guide.

## What this is

MOSAIC (Multi-camera Observatory for Social & Activity Interaction Capture) is a research tool
for recording and analysing social interactions in a lab room:
- **Acquisition:** a Qt 6 / C++23 desktop app (`src/`) that records up to six hardware-triggered
  Basler GigE cameras, microphones and trigger events, with live preview, calibration, profiles
  and a session browser.
- **Analysis:** Python plugins (`analysis/`) run from the app's Analysis tab:
  - pose, 3D pose and gaze fusion;
  - face dynamics, expression, eye contact, conversation timing, diarization and transcription;
  - remote heart rate (rPPG), voice and motion;
  - sync repair and session reports.
- **Docs:** Sphinx + Doxygen (`docs/`), with a `docs/math/` page per method.

Data come from real participants, so correctness, privacy and honest reporting matter more than
speed.

## Working with the human (read first)

- **Never `git add`, `git commit` or `git push`.** Prepare the change, run the checks, then give
  the human the exact commands. They commit and push themselves. Agents may create branches,
  open PRs with `gh pr create` once the human has pushed, and delete branches that are fully
  merged. Before deleting, check `git log origin/main..origin/<branch>` is empty: merged PRs can
  miss main.
- **Commit trailer:** end commit messages with
  `Co-Authored-By: <the model's name> <noreply@anthropic.com>` (or the equivalent for the tool
  used), and end PR descriptions with a line saying the tool that generated it. MOSAIC keeps AI
  co-authorship visible in the history rather than only in PR text. The human who pushes is
  still responsible for every line and must be able to explain it.
- **One branch per change**, off an up-to-date `main` (`fix/...`, `feature/...`, `docs/...`).
  Do not stack PRs on unmerged branches.
- **Sketch before building** anything beyond a local fix: a few sentences on the design, the
  files it touches and how it is tested. Ask before building items the human has only put on the
  plan.

## Keep changes small

A big diff is a cost. Every line is reviewed, then maintained by a small lab team.

- **YAGNI:** no options, config keys, schema versions, retries or extension points that nothing
  needs yet. Hard-code the one behaviour that is wanted.
- **DRY:** look for existing helpers first, for example `analysis/*/` shared modules, the
  `room_frame` maths, `MetricsChartW`, the result readers in `src/analysis/`, the plugin registry
  `src/analysis/analysis_plugins.hpp`. Factor out rather than copy.
- **Extend existing tests before adding new files:** `tests/` (GoogleTest) and
  `analysis/tests/` (pytest; `golden_session.py` builds a small real-shaped session).
- **Never add a dependency, or promote an optional one, without asking.**

## Hard boundaries

Never:
- **launch `mosaic.exe`** or anything that opens the room's real cameras;
- **move, edit or delete anything under `recordings/`** (or `build/**/recordings/`): it is
  participant data. Read only, and run analyses on a scratch copy;
- **read, decrypt or print credentials** (the Hugging Face token is DPAPI-encrypted in settings);
- **skip, loosen or delete a test** to make a change pass, bypass hooks (`--no-verify`), or
  silence lint instead of fixing the cause;
- **install software or change system settings** (NIC, drivers, Pylon) without asking.

Ask first before:
- adding or bumping a dependency (`analysis/pyproject.toml`, `vcpkg.json`, ...);
- editing CI (`.github/workflows/`), `.pre-commit-config.yaml`, `ruff.toml`, `.clang-format`
  or `.clang-tidy`;
- changing a file format other tools read (`session_meta.json`, `sync_manifest.json`, result
  JSON schemas, `settings.json` keys) in a way that is not purely additive.

## Common commands

Windows, Git Bash. Builds use the existing build tree; `scripts/configure.ps1` makes a new one.

```bash
# C++ build and tests
MSYS_NO_PATHCONV=1 cmake --build build/Release --config Release --parallel
cd build/Release && ctest -C Release -j4 --output-on-failure
# (the SessionArchive tests can fail under heavy parallel load; rerun them alone)

# Python analysis tests (from analysis/)
.venv/Scripts/python -m pytest -q
.venv/Scripts/python -m pytest -q tests/test_pose3d_audit.py

# Formatting and lint (from the repo root; these are the CI versions)
uvx ruff@0.8.4 format --config ruff.toml .
uvx ruff@0.8.4 check --config ruff.toml .
clang-format -i <changed .cpp/.hpp files>        # 18.1.8, the repo's .clang-format

# Docs (from docs/; a scratch output folder, then clean the generated api/ pages)
DOXYGEN_XML=D:/mosaic/build/docs_check/doxygen/xml uv run sphinx-build -b html --keep-going -q . <scratch>
git checkout -- api; git clean -fdq api
```

**Before handing work back:**
- build;
- run ctest and pytest;
- run ruff and clang-format **over every changed file, again after the last edit**;
- build the docs when `.rst` or public docstrings changed;
- review the diff (or have a review agent do it) and fix what it finds.

Report failures as they are; do not work around them.

## Code conventions

- **C++:** C++23, Qt 6, `.clang-format`. OpenCV code inside `#if defined(MOSAIC_HAVE_OPENCV)`.
  Heavy work off the GUI thread; results handed back with a queued `QMetaObject::invokeMethod`.
  New sources are listed in `src/CMakeLists.txt`, and in `tests/CMakeLists.txt` when tested.
- **Python:** each of `python/`, `analysis/` and `docs/` is its own uv project. Analysis entry
  points are `analysis/run_<name>.py`, writing result JSON or CSV into the session folder. The C++
  side reads it in `src/analysis/<name>_result.*` and registers the plugin in
  `analysis_plugins.hpp`.
- **Line endings:** files are CRLF in the working tree (`core.autocrlf=true`). When patching
  with scripts, keep each file's existing line endings.
- **Comments:** explain why, in plain sentences, at the density of the surrounding code.
- **Docs:** a user-visible change updates `docs/user_guide.rst` (or `calibration.rst`,
  `recording.rst`, ...). A new method gets or updates its `docs/math/` page: what is computed,
  why each choice, limits. **No em dashes** in the docs. Credit borrowed methods and their
  licence (e.g. the MIT 4by4k pipeline).

## Hardware and data

- CI builds without cameras or FFmpeg. Changes touching cameras, encoding, triggers or sync need
  a room-11 check: add a row to the room test tracker and describe the check in the PR's
  "Room check" section.
- Settings are per profile (`%LOCALAPPDATA%/CSRU/MOSAIC/settings.json` and
  `profiles/<user>/settings.json`, some with a UTF-8 BOM). They are written when MOSAIC closes.
  Back up before editing one by hand.
- Ultralytics and other model downloads land in the working folder: `*.pt` is git-ignored.

## PRs

- **Title:** what changes, for a user.
- **Body sections:** Why; What changes; Review (what a review found and what was fixed); Tests;
  Room check (when hardware is involved).
- Write the body to a file and use `gh pr create --body-file`.
