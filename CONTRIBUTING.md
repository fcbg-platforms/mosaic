# Contributing to Mosaic

## Workflow

- All changes land via a pull request against `main` — no direct pushes to `main` except for
  initial repository setup.
- Branch naming: short, descriptive (e.g. `fix/camera-live-apply`, `feat/eeg-trigger-sync`).
- Keep PRs scoped to one roadmap item or bug where practical; large items should be split into
  reviewable stages rather than one sweeping PR.

## Before opening a PR

- [ ] **Add or update a test** for any new or changed behavior. This is a hard requirement, not
      a nice-to-have — untested code paths are the main way regressions have slipped in
      previously (see `tests/`, GoogleTest via `ctest`).
- [ ] Run the local build with `MOSAIC_BUILD_TESTS=ON` and confirm `ctest --output-on-failure`
      passes.
- [ ] CI (`.github/workflows/ci.yml`) builds the hardware-free configuration
      (`MOSAIC_ENABLE_CAMERAS=OFF`, `MOSAIC_ENABLE_FFMPEG=OFF`) — it cannot exercise camera,
      encoding, or parallel-port hardware paths. Changes touching those areas need a manual note
      in the PR description describing how they were verified locally
      (e.g. "tested live against 6 physical cameras in room 11").
- [ ] Python changes (`python/`, `analysis/`, `docs/`) pass `ruff check` / `ruff format` (shared
      config at repo-root `ruff.toml`; each directory is its own uv project — see README's
      [Python environments](README.md#python-environments)). Installing the
      `.pre-commit-config.yaml` hooks runs this automatically. CI enforces both via the
      `lint-python` job.
- [ ] C++ changes (`src/`, `tests/`) pass `clang-format --dry-run --Werror` against the repo-root
      `.clang-format`. CI enforces this via the `lint-cpp` job. `.clang-tidy` is also configured
      in the repo but is **not yet CI-enforced** — running it for real needs a valid
      `compile_commands.json` from a full Qt+vcpkg build, which would make a "just check style"
      job as heavy as the full build job; treat it as a manual/local check for now.
- [ ] No unrelated file churn — check `git status`/`git diff` before staging.

## Code style

- C++: follow `.clang-format`/`.clang-tidy` already configured in the repo (`.clang-format` is
  CI-enforced; `.clang-tidy` is not yet, see above).
- Python: `ruff`, config at repo-root `ruff.toml`, shared across `python/`, `analysis/`, `docs/`.

## Reporting issues / proposing features

Open a GitHub issue describing the problem or proposal before starting large or speculative
work, so design questions can be resolved before code is written.
