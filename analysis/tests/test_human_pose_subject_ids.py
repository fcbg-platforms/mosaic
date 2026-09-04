"""Pure-logic tests for pose/human_pose.py's subject-id assignment and tracker
reset — no ultralytics, torch or model weights needed.

human_pose.py guards its ultralytics import (``_ULTRALYTICS_OK``), so both
``resolve_subject_ids()`` and ``_reset_model_trackers()`` are importable on a
bare env. Same "extract the one testable piece" pattern that
pose3d/tracker.py's PersonTracker3D and diarize's resolve_device() established.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from pose.human_pose import _reset_model_trackers, resolve_subject_ids


def test_tracker_ids_are_used_verbatim():
    # The whole point: a tracker id means "the same physical person", so it
    # must survive unchanged into the .pose.json the C++ side looks up by.
    assert resolve_subject_ids([4, 7], 2) == [4, 7]


def test_no_tracker_ids_falls_back_to_distinct_negatives():
    # -1 for every detection would collapse several untracked people onto one
    # subject id, which is worse than the detection-order bug being replaced —
    # the C++ side looks subjects up *by* id.
    assert resolve_subject_ids(None, 3) == [-1, -2, -3]


def test_missing_ids_inside_the_list_only_affect_their_own_index():
    assert resolve_subject_ids([5, None, 9], 3) == [5, -2, 9]


def test_non_positive_ids_are_treated_as_untracked():
    # Ultralytics track ids start at 1, so 0 or negative means "not a real
    # track" however it arrived.
    assert resolve_subject_ids([0, -3], 2) == [-1, -2]


def test_short_id_list_keeps_leading_ids_and_does_not_shift():
    # Resolved per index rather than by zipping: a shift would attach one
    # person's keypoints to another person's id.
    assert resolve_subject_ids([11], 3) == [11, -2, -3]


def test_long_id_list_is_truncated_to_the_detection_count():
    assert resolve_subject_ids([1, 2, 3, 4], 2) == [1, 2]


def test_zero_detections_yields_no_ids():
    assert resolve_subject_ids([1, 2], 0) == []
    assert resolve_subject_ids(None, 0) == []


def test_ids_are_always_distinct():
    # find_subject() on the C++ side returns the first match for an id, so a
    # duplicate would silently hide one person behind another.
    shapes = [
        (None, 4),
        ([1, None, None], 3),
        ([2, 0, 5], 3),
        ([], 3),
        ([3], 4),
    ]
    for track_ids, count in shapes:
        out = resolve_subject_ids(track_ids, count)
        assert len(out) == count, (track_ids, count)
        assert len(set(out)) == len(out), out


def test_ids_are_plain_ints_not_numpy_or_torch_scalars():
    # json.dumps() in run_pose.py's _write_results() cannot serialize a torch
    # or numpy scalar, and the failure would only show at the very end of a
    # long analysis run.
    out = resolve_subject_ids([1, None], 2)
    assert all(type(v) is int for v in out)


class _FakeTracker:
    def __init__(self):
        self.reset_calls = 0

    def reset(self):
        self.reset_calls += 1


class _FakePredictor:
    def __init__(self, trackers):
        self.trackers = trackers


class _FakeModel:
    def __init__(self, predictor=None):
        self.predictor = predictor


def test_reset_clears_every_attached_tracker():
    trackers = [_FakeTracker(), _FakeTracker()]
    model = _FakeModel(_FakePredictor(trackers))

    assert _reset_model_trackers(model) is True
    assert [t.reset_calls for t in trackers] == [1, 1]


def test_reset_reports_false_when_there_is_nothing_attached():
    # Both real pre-track() states: no predictor at all, and a predictor whose
    # trackers list is empty. reset_tracker() falls back to dropping the
    # predictor in these cases, so it must be able to tell them apart from a
    # successful reset.
    assert _reset_model_trackers(_FakeModel(None)) is False
    assert _reset_model_trackers(_FakeModel(_FakePredictor([]))) is False
    assert _reset_model_trackers(object()) is False
