"""
Regression test against the golden session (see golden_session.py): the
plugins' output must keep the committed shape, and the session's headline
numbers must stay close to the committed ones.

A failure here means a plugin's output changed. If that was deliberate,
update the C++ readers if needed and refresh the golden copy with::

    python analysis/tests/golden_session.py --update
"""

import json
import math
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent.parent))

import golden_session

UPDATE_HINT = "run `python analysis/tests/golden_session.py --update` if the change was deliberate"


def shape(value):
    """The structure of a JSON value: key trees and value kinds, with every
    distinct element shape of a list (so optional keys and nulls count)."""
    if isinstance(value, dict):
        return {k: shape(v) for k, v in sorted(value.items())}
    if isinstance(value, list):
        seen = {json.dumps(shape(v), sort_keys=True) for v in value}
        return sorted(seen)
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, int | float):
        return "number"
    if value is None:
        return "null"
    return type(value).__name__


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    return golden_session.build(tmp_path_factory.mktemp("golden") / "session")


def test_the_golden_copy_is_committed():
    for rel in golden_session.KEEP:
        assert (golden_session.GOLDEN / rel).exists(), f"missing golden file {rel}: {UPDATE_HINT}"


@pytest.mark.parametrize("rel", [r for r in golden_session.KEEP if r.endswith(".json")])
def test_output_shape_is_unchanged(built, rel):
    new = json.loads((built / rel).read_text(encoding="utf-8"))
    old = json.loads((golden_session.GOLDEN / rel).read_text(encoding="utf-8"))
    assert new.get("schema") == old.get("schema"), f"{rel}: schema name changed; {UPDATE_HINT}"
    assert shape(new) == shape(
        old
    ), f"{rel}: the output's keys or value types changed; {UPDATE_HINT}"


def test_headline_numbers_are_stable(built):
    """The session summary row: every number within 5% (or 1 unit for
    counts and small values) of the committed one, every text equal."""
    rel = "report/session_summary.json"
    new = json.loads((built / rel).read_text(encoding="utf-8"))
    old = json.loads((golden_session.GOLDEN / rel).read_text(encoding="utf-8"))
    assert set(new) == set(old), f"summary columns changed; {UPDATE_HINT}"
    moved = []
    for key, was in old.items():
        now = new[key]
        if (
            isinstance(was, int | float)
            and isinstance(now, int | float)
            and not isinstance(was, bool)
        ):
            if not math.isclose(now, was, rel_tol=0.05, abs_tol=1.0):
                moved.append(f"{key}: {was} -> {now}")
        elif now != was:
            moved.append(f"{key}: {was!r} -> {now!r}")
    assert not moved, "headline numbers moved:\n  " + "\n  ".join(moved) + f"\n{UPDATE_HINT}"


def test_inputs_are_unchanged(built):
    for rel in ("session_meta.json", "video/timestamps_cam2.csv", "audio/audio.transcript.json"):
        assert (built / rel).read_text(encoding="utf-8") == (golden_session.GOLDEN / rel).read_text(
            encoding="utf-8"
        ), f"{rel}: the synthetic input changed; {UPDATE_HINT}"


def test_shape_sees_a_renamed_key_and_a_new_type():
    assert shape({"a": 1}) != shape({"b": 1})
    assert shape({"a": 1}) != shape({"a": "1"})
    assert shape({"a": [1, None]}) != shape({"a": [1]})  # a list item became nullable
    assert shape({"a": 1}) == shape({"a": 2.5})  # int and float are both numbers
