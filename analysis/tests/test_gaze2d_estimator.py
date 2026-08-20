"""
Pure-logic tests for gaze2d/estimator.py's compute_iris_offset()/
gaze_on_target() — no mediapipe/cv2 import required.

Neither of this heuristic's two other existing copies
(python/pose/gaze_estimator.py's GazeEstimator.estimate(),
analysis/gaze/estimator.py's _iris_offset()) has ever had a dedicated unit
test despite being duplicated twice already — this is the first real test
of the actual math, made possible here specifically because
compute_iris_offset() is pulled out as a non-underscore-prefixed, directly
testable function taking a plain list of (x, y)-like objects, no
mediapipe/cv2 dependency required to exercise it.
"""

import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from gaze2d.estimator import compute_iris_offset, gaze_on_target


@dataclass
class _Lm:
    x: float
    y: float


def _make_landmarks(
    left_iris=(0.35, 0.40),
    right_iris=(0.65, 0.40),
    left_outer=(0.30, 0.40),
    left_inner=(0.40, 0.40),
    left_top=(0.35, 0.37),
    left_bottom=(0.35, 0.43),
    right_inner=(0.60, 0.40),
    right_outer=(0.70, 0.40),
    right_top=(0.65, 0.37),
    right_bottom=(0.65, 0.43),
) -> list[_Lm]:
    """Builds a 478-entry landmark list with only the indices
    compute_iris_offset() actually reads populated meaningfully — every
    other index is a harmless placeholder at the origin, since the
    function never touches them."""
    lm = [_Lm(0.0, 0.0) for _ in range(478)]
    lm[468] = _Lm(*left_iris)
    lm[473] = _Lm(*right_iris)
    lm[33] = _Lm(*left_outer)
    lm[133] = _Lm(*left_inner)
    lm[159] = _Lm(*left_top)
    lm[145] = _Lm(*left_bottom)
    lm[362] = _Lm(*right_inner)
    lm[263] = _Lm(*right_outer)
    lm[386] = _Lm(*right_top)
    lm[374] = _Lm(*right_bottom)
    return lm


def test_iris_exactly_centered_gives_near_zero_offset():
    lm = _make_landmarks(left_iris=(0.35, 0.40), right_iris=(0.65, 0.40))
    dx, dy = compute_iris_offset(lm)
    assert dx is not None
    assert abs(dx) < 1e-9
    assert abs(dy) < 1e-9


def test_iris_shifted_right_gives_positive_dx():
    # Both eye-socket centers are at x=0.35/0.65 (eye width 0.10); shift
    # both irises 0.03 to the right of their own socket center.
    lm = _make_landmarks(left_iris=(0.38, 0.40), right_iris=(0.68, 0.40))
    dx, dy = compute_iris_offset(lm)
    assert dx is not None
    assert dx > 0.0
    assert abs(dy) < 1e-9


def test_iris_shifted_up_gives_negative_dy():
    lm = _make_landmarks(left_iris=(0.35, 0.37), right_iris=(0.65, 0.37))
    dx, dy = compute_iris_offset(lm)
    assert dy is not None
    assert dy < 0.0


def test_degenerate_zero_eye_width_returns_none_none():
    lm = _make_landmarks(
        left_outer=(0.35, 0.40),
        left_inner=(0.35, 0.40),
        right_outer=(0.65, 0.40),
        right_inner=(0.65, 0.40),
    )
    dx, dy = compute_iris_offset(lm)
    assert dx is None
    assert dy is None


def test_extreme_offset_is_clamped_to_unit_range():
    # Iris centre placed far outside the eye socket entirely — offset
    # magnitude would exceed 1.0 without clamping.
    lm = _make_landmarks(left_iris=(2.0, 0.40), right_iris=(0.65, 0.40))
    dx, dy = compute_iris_offset(lm)
    assert dx is not None
    assert -1.0 <= dx <= 1.0
    assert -1.0 <= dy <= 1.0


def test_gaze_on_target_within_threshold():
    assert gaze_on_target(0.1, -0.2) is True
    assert gaze_on_target(0.35, 0.35) is True  # exactly at the boundary


def test_gaze_on_target_outside_threshold():
    assert gaze_on_target(0.5, 0.0) is False
    assert gaze_on_target(0.0, -0.4) is False


def test_gaze_on_target_custom_threshold():
    assert gaze_on_target(0.6, 0.6, threshold=0.7) is True
    assert gaze_on_target(0.6, 0.6, threshold=0.5) is False
