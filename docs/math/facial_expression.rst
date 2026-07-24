Facial Expression
====================

.. contents:: On this page
   :local:
   :depth: 2

Implemented in :mod:`expression.classifier` (the default heuristic
backend) and :mod:`expression.ferplus` (the pretrained FER+ backend) — see
:doc:`/analysis_api` for the full API reference.

Heuristic backend — weighted blendshape scoring
-----------------------------------------------------

:func:`~expression.classifier.classify_expression` scores each of 7 basic
emotion categories as the **weighted mean** (not sum) of a curated,
FACS-Action-Unit-derived subset of MediaPipe's 52 blendshape scores:

.. math::

   \text{score}_c = \frac{\sum_{k} w_{c,k} \, b_k}{\sum_{k} w_{c,k}}

where :math:`b_k` is blendshape :math:`k`'s activation in :math:`[0, 1]`
and :math:`w_{c,k}` is category :math:`c`'s weight for that blendshape
(``CATEGORY_WEIGHTS``). Using the **mean** rather than the sum means a
category referencing many blendshapes isn't unfairly favored over one
referencing few — every category's score stays comparable in
:math:`[0, 1]` regardless of how many terms it sums.

The winning category is the argmax over all 7 scores, with two guards:

- **Neutral fallback.** If the winning score is below a fixed threshold
  (``_MIN_ACTIVATION = 0.15``), the result is forced to ``"Neutral"``
  regardless of which category technically won — weak, ambiguous evidence
  shouldn't produce a confident emotion label.
- **Tie-break.** ``"Neutral"`` is listed first in ``CATEGORIES``, and an
  exact tie keeps the first-seen maximum — a deliberate "when in doubt,
  don't overclaim an emotion" default.

FER+ backend — softmax
---------------------------

:func:`~expression.ferplus._softmax_and_label` converts the FER+ ONNX
model's 8 raw logits :math:`z_1, \ldots, z_8` into a probability
distribution over its 8 emotion categories via the standard
numerically-stable softmax:

.. math::

   p_i = \frac{e^{z_i - \max_j z_j}}{\sum_{k} e^{z_k - \max_j z_j}}

Subtracting :math:`\max_j z_j` before exponentiating doesn't change the
result (it cancels in the ratio) but keeps every exponent :math:`\le 0`,
avoiding floating-point overflow for large logits. The reported label is
:math:`\arg\max_i p_i`, with :math:`p_i` itself reported as the
confidence score.
