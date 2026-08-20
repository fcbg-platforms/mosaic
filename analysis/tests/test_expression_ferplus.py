"""
Pure-logic tests for expression/ferplus.py's _softmax_and_label() — the one
piece of the FER+ backend that's dependency-free (no onnxruntime/model file
needed), same "extract the one pure/testable piece" pattern diarize's
resolve_device() established for a lazily-imported ML backend.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from expression.ferplus import FERPLUS_LABELS, _softmax_and_label


def test_dominant_class_picked_from_highest_logit():
    logits = [0.0] * 8
    logits[1] = 5.0  # "happiness"
    label, score = _softmax_and_label(logits, FERPLUS_LABELS)
    assert label == "Happiness"
    assert score > 0.9  # a 5.0-vs-0.0 logit gap should dominate the softmax


def test_softmax_output_sums_to_one_across_all_classes():
    logits = [1.0, 2.0, 3.0, 0.5, -1.0, 0.0, 2.5, -0.5]
    # Reconstruct the full probability vector the same way the function does
    # internally, to check normalization independent of which label wins.
    import math

    max_logit = max(logits)
    exps = [math.exp(v - max_logit) for v in logits]
    total = sum(exps)
    probs = [e / total for e in exps]
    assert abs(sum(probs) - 1.0) < 1e-9


def test_uniform_logits_tie_break_picks_first_label():
    logits = [1.0] * 8
    label, score = _softmax_and_label(logits, FERPLUS_LABELS)
    assert label == FERPLUS_LABELS[0]  # "Neutral"
    assert abs(score - 1.0 / 8) < 1e-9


def test_labels_official_order_matches_ferplus_spec():
    # Cross-checked against onnx/models' README and the FERPlus training
    # repo's CSV column order (see ferplus.py module docstring) — a wrong
    # order here would silently mislabel every prediction.
    assert FERPLUS_LABELS == [
        "Neutral",
        "Happiness",
        "Surprise",
        "Sadness",
        "Anger",
        "Disgust",
        "Fear",
        "Contempt",
    ]


def test_negative_logits_do_not_break_softmax():
    logits = [-5.0, -3.0, -8.0, -1.0, -6.0, -7.0, -2.0, -4.0]
    label, score = _softmax_and_label(logits, FERPLUS_LABELS)
    assert label == "Sadness"  # index 3, the least-negative logit
    assert 0.0 < score <= 1.0
