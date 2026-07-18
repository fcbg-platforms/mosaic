"""
Rule-based blendshape -> dominant-expression classifier ("heuristic"
backend). Pure, dependency-free, zero I/O — this is the highest-value
function to get right in this plugin (a bug here silently mislabels every
detected face's expression), the same severity class as facemask's
expand_and_clip() and diarize's assign_speakers(). See
analysis/tests/test_expression_classifier.py.

Each of the 7 basic-emotion categories is backed by a small set of ARKit
blendshape names with weights, chosen from well-established FACS
Action-Unit -> blendshape correspondences (documented per-category below).
Neutral uses MediaPipe's own "_neutral" output category directly rather
than reinventing a neutral signal from the other 51 scores — it's already a
purpose-built "how neutral is this face" signal.

Known limitation, deliberately not "fixed" in this first cut: Surprised and
Fearful share most of their weighted blendshapes (brow raisers + eye
widening) and are only distinguished by jawOpen (Surprised) vs.
mouthStretchLeft/Right (Fearful) — real faces can activate both
simultaneously, so misclassification between these two specific categories
is expected and acceptable.
"""
from __future__ import annotations

CATEGORIES: list[str] = [
    "Neutral", "Happy", "Sad", "Surprised", "Angry", "Disgusted", "Fearful",
]

# {category: {blendshape_name: weight}}. Weighted MEAN (not sum) is taken
# per category at classification time, so a category listing 6 blendshapes
# isn't unfairly favored over one listing 2 — every category's score stays
# comparable in [0, 1] regardless of how many shapes it references.
CATEGORY_WEIGHTS: dict[str, dict[str, float]] = {
    "Neutral": {
        "_neutral": 1.0,
    },
    "Happy": {                      # AU6 (cheek raiser) + AU12 (lip corner puller)
        "mouthSmileLeft": 1.0, "mouthSmileRight": 1.0,
        "cheekSquintLeft": 0.3, "cheekSquintRight": 0.3,
        "mouthDimpleLeft": 0.2, "mouthDimpleRight": 0.2,
    },
    "Sad": {                        # AU1 (inner brow raiser) + AU15 (lip corner depressor)
        "mouthFrownLeft": 1.0, "mouthFrownRight": 1.0,
        "browInnerUp": 0.6,
        "mouthLowerDownLeft": 0.2, "mouthLowerDownRight": 0.2,
    },
    "Surprised": {                  # AU1+AU2 (brow raisers) + AU5 (lid raiser) + AU26 (jaw drop)
        "jawOpen": 0.8,
        "browInnerUp": 0.7,
        "browOuterUpLeft": 0.7, "browOuterUpRight": 0.7,
        "eyeWideLeft": 0.5, "eyeWideRight": 0.5,
    },
    "Angry": {                      # AU4 (brow lowerer) + AU7 (lid tightener) + AU23 (lip tight.)
        "browDownLeft": 1.0, "browDownRight": 1.0,
        "eyeSquintLeft": 0.4, "eyeSquintRight": 0.4,
        "mouthPressLeft": 0.3, "mouthPressRight": 0.3,
        "noseSneerLeft": 0.2, "noseSneerRight": 0.2,
    },
    "Disgusted": {                  # AU9 (nose wrinkler) + AU10 (upper lip raiser)
        "noseSneerLeft": 1.0, "noseSneerRight": 1.0,
        "mouthUpperUpLeft": 0.6, "mouthUpperUpRight": 0.6,
        "browDownLeft": 0.2, "browDownRight": 0.2,
    },
    "Fearful": {                    # AU1+AU2+AU5 (like Surprise) + AU20 (lip stretch, no jaw drop)
        "browInnerUp": 0.6,
        "browOuterUpLeft": 0.5, "browOuterUpRight": 0.5,
        "eyeWideLeft": 0.7, "eyeWideRight": 0.7,
        "mouthStretchLeft": 0.6, "mouthStretchRight": 0.6,
    },
}

# The winning category's weighted-mean score below this is treated as
# insufficient evidence -> forced to Neutral regardless of argmax.
_MIN_ACTIVATION = 0.15


def classify_expression(blendshape_names: list[str],
                         blendshape_scores: list[float]) -> tuple[str, float]:
    """Picks the dominant basic-emotion category from a parallel
    (blendshape_names, blendshape_scores) pair. Unknown/missing blendshape
    names default to 0.0 (robust to a caller passing a truncated array).

    Tie-break: CATEGORIES lists "Neutral" first, and Python's max() keeps
    the first-seen maximum on an exact tie among the 7 categories — a
    deliberate "when in doubt, don't overclaim an emotion" default,
    consistent with diarize's assign_speakers() leaving speaker=None on a
    zero-overlap tie.

    Returns (dominant_expression, dominant_score); score is always in
    [0, 1].
    """
    lookup = dict(zip(blendshape_names, blendshape_scores))

    category_scores: dict[str, float] = {}
    for category, weights in CATEGORY_WEIGHTS.items():
        total_weight = sum(weights.values())
        weighted_sum = sum(lookup.get(name, 0.0) * w for name, w in weights.items())
        category_scores[category] = weighted_sum / total_weight if total_weight > 0 else 0.0

    best_category = max(CATEGORIES, key=lambda c: category_scores[c])
    best_score = category_scores[best_category]

    if best_score < _MIN_ACTIVATION:
        return "Neutral", category_scores["Neutral"]
    return best_category, best_score
