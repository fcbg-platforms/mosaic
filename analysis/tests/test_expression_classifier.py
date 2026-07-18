"""
Pure-logic tests for expression/classifier.py's classify_expression() — no
mediapipe/onnxruntime import required, since the function has zero
dependencies beyond the standard library.

classify_expression() is the highest-value function to get right in this
feature: a bug here silently mislabels every detected face's expression, the
same severity class as facemask's expand_and_clip() and diarize's
assign_speakers().
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from expression.classifier import CATEGORIES, classify_expression
from expression.detector import BLENDSHAPE_NAMES


def _scores(**overrides) -> tuple[list[str], list[float]]:
    """Builds a full-length parallel array with 0.0 everywhere except the
    given overrides — avoids every test needing to enumerate all 52 names."""
    values = [overrides.get(name, 0.0) for name in BLENDSHAPE_NAMES]
    return BLENDSHAPE_NAMES, values


def test_all_zero_defaults_to_neutral():
    names, scores = _scores()
    label, _ = classify_expression(names, scores)
    assert label == "Neutral"


def test_strong_neutral_signal_wins():
    names, scores = _scores(_neutral=0.9)
    label, score = classify_expression(names, scores)
    assert label == "Neutral"
    assert score > 0.5


def test_smile_activates_happy():
    names, scores = _scores(mouthSmileLeft=0.9, mouthSmileRight=0.9,
                             cheekSquintLeft=0.5, cheekSquintRight=0.5)
    label, score = classify_expression(names, scores)
    assert label == "Happy"
    assert 0.0 < score <= 1.0


def test_frown_and_inner_brow_activates_sad():
    names, scores = _scores(mouthFrownLeft=0.8, mouthFrownRight=0.8, browInnerUp=0.6)
    assert classify_expression(names, scores)[0] == "Sad"


def test_jaw_open_and_brow_raise_activates_surprised():
    names, scores = _scores(jawOpen=0.9, browInnerUp=0.8,
                             browOuterUpLeft=0.8, browOuterUpRight=0.8,
                             eyeWideLeft=0.6, eyeWideRight=0.6)
    assert classify_expression(names, scores)[0] == "Surprised"


def test_brow_down_and_lip_press_activates_angry():
    names, scores = _scores(browDownLeft=0.9, browDownRight=0.9,
                             eyeSquintLeft=0.5, eyeSquintRight=0.5,
                             mouthPressLeft=0.4, mouthPressRight=0.4)
    assert classify_expression(names, scores)[0] == "Angry"


def test_nose_sneer_and_upper_lip_raise_activates_disgusted():
    names, scores = _scores(noseSneerLeft=0.9, noseSneerRight=0.9,
                             mouthUpperUpLeft=0.7, mouthUpperUpRight=0.7)
    assert classify_expression(names, scores)[0] == "Disgusted"


def test_mouth_stretch_without_jaw_open_activates_fearful_not_surprised():
    names, scores = _scores(browInnerUp=0.7, browOuterUpLeft=0.6, browOuterUpRight=0.6,
                             eyeWideLeft=0.8, eyeWideRight=0.8,
                             mouthStretchLeft=0.8, mouthStretchRight=0.8)   # no jawOpen
    assert classify_expression(names, scores)[0] == "Fearful"


def test_weak_signals_below_min_activation_fall_back_to_neutral():
    names, scores = _scores(mouthSmileLeft=0.1, mouthSmileRight=0.1)
    label, _ = classify_expression(names, scores)
    assert label == "Neutral"


def test_result_is_always_one_of_the_defined_categories():
    # Two categories tuned to a near-identical weighted mean — the important
    # behavior to lock in is that classify_expression() never raises and
    # always returns a value from CATEGORIES, not a brittle exact tie label.
    names, scores = _scores(mouthSmileLeft=1.0, mouthSmileRight=1.0,
                             mouthFrownLeft=1.0, mouthFrownRight=1.0)
    label, score = classify_expression(names, scores)
    assert label in CATEGORIES
    assert 0.0 <= score <= 1.0


def test_missing_blendshape_names_default_to_zero_without_raising():
    # A truncated/partial names array (e.g. malformed input) must not KeyError.
    label, score = classify_expression(["mouthSmileLeft"], [0.9])
    assert label in CATEGORIES
    assert 0.0 <= score <= 1.0
