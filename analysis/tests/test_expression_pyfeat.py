"""
Pure-logic tests for expression/pyfeat.py's _fex_row_to_result() — the one
piece of the py-feat backend that's dependency-free (no torch/torchcodec/
feat installed needed), same "extract the one pure/testable piece" pattern
ferplus.py's _softmax_and_label() and diarize's resolve_device() established
for a lazily-imported ML backend.

Importantly, this file itself must be importable WITHOUT torch/torchcodec/
feat installed — that's the whole point of pyfeat.py deferring `from feat
import Detectorv1`/`import torch`/`import cv2` into PyFeatClassifier
methods rather than the module top level.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from expression.pyfeat import AU_NAMES, _fex_row_to_result


def test_dominant_emotion_picked_from_highest_score():
    emotions = {
        "anger": 0.1,
        "disgust": 0.05,
        "fear": 0.05,
        "happiness": 0.9,
        "sadness": 0.02,
        "surprise": 0.1,
        "neutral": 0.2,
    }
    label, score, _ = _fex_row_to_result({}, emotions)
    assert label == "Happiness"
    assert abs(score - 0.9) < 1e-9


def test_emotion_labels_are_title_case_not_raw_column_names():
    emotions = {
        "anger": 0.0,
        "disgust": 0.0,
        "fear": 0.0,
        "happiness": 0.0,
        "sadness": 1.0,
        "surprise": 0.0,
        "neutral": 0.0,
    }
    label, _, _ = _fex_row_to_result({}, emotions)
    assert label == "Sadness"  # not the raw "sadness" column name


def test_au_dict_passthrough_rounds_to_4_decimals():
    aus = {"AU01": 0.123456, "AU06": 0.9}
    _, _, au_values = _fex_row_to_result(aus, {"neutral": 1.0})
    assert au_values == {"AU01": 0.1235, "AU06": 0.9}


def test_nan_emotion_value_treated_as_zero_not_propagated():
    nan = float("nan")
    emotions = {
        "anger": nan,
        "disgust": 0.0,
        "fear": 0.0,
        "happiness": 0.3,
        "sadness": 0.0,
        "surprise": 0.0,
        "neutral": 0.0,
    }
    label, score, _ = _fex_row_to_result({}, emotions)
    assert label == "Happiness"  # the NaN entry must not win or crash argmax
    assert abs(score - 0.3) < 1e-9


def test_nan_au_value_treated_as_zero_not_propagated():
    nan = float("nan")
    aus = {"AU01": nan, "AU06": 0.5}
    _, _, au_values = _fex_row_to_result(aus, {"neutral": 1.0})
    assert au_values == {"AU01": 0.0, "AU06": 0.5}


def test_empty_emotion_values_defaults_to_neutral_zero():
    label, score, au_values = _fex_row_to_result({}, {})
    assert label == "Neutral"
    assert score == 0.0
    assert au_values == {}


def test_au_names_has_20_entries_matching_detectorv1_xgb_head():
    # Verified against feat/pretrained.py's AU_LANDMARK_MAP["Feat"] — a
    # wrong count/order here would silently mislabel every AU column.
    assert len(AU_NAMES) == 20
    assert AU_NAMES[0] == "AU01"
    assert AU_NAMES == [
        "AU01",
        "AU02",
        "AU04",
        "AU05",
        "AU06",
        "AU07",
        "AU09",
        "AU10",
        "AU11",
        "AU12",
        "AU14",
        "AU15",
        "AU17",
        "AU20",
        "AU23",
        "AU24",
        "AU25",
        "AU26",
        "AU28",
        "AU43",
    ]
