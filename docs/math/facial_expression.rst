Facial Expression
====================

.. contents:: On this page
   :local:
   :depth: 2

Implemented in :mod:`expression.classifier` (the default heuristic
backend), :mod:`expression.ferplus` (the pretrained FER+ backend), and
:mod:`expression.pyfeat` (the pretrained py-feat backend) — see
:doc:`/analysis_api` for the full API reference. All three consume the
same MediaPipe FaceLandmarker-detected blendshapes/face crop and expose the
same ``(dominant_label, dominant_score)`` shape, so results are directly
comparable across backends for the same session.

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

py-feat backend — FACS Action Units + emotion
---------------------------------------------------

:class:`~expression.pyfeat.PyFeatClassifier` wraps py-feat's ``Detectorv1``
(deliberately not the newer ``Detectorv2``, whose default weights carry a
research-only, non-commercial license — ``Detectorv1``'s default
``au_model="xgb"``/``emotion_model="resmasknet"`` heads carry no such
restriction). This is the most detailed of the three backends: unlike the
heuristic and FER+ backends, which only ever report a single dominant
category, py-feat additionally reports **20 individual FACS Action Units**
(:data:`~expression.pyfeat.AU_NAMES` — e.g. AU12 = lip corner puller/smile,
AU04 = brow lowerer), each a continuous, independently-calibrated
probability :math:`p_{\text{AU}} \in [0, 1]`.

.. important::

   These AU values are **calibrated probabilities in** :math:`[0, 1]`,
   **not** the classic FACS 0–5 ordinal intensity scale researchers may be
   used to from manual FACS coding — a value of ``0.8`` means "the
   detector is 80% confident this Action Unit is active," not "intensity
   level 4 of 5."

Dominant-emotion selection is a plain argmax over py-feat's own 7-category
``resmasknet`` emotion output (mapped to Title-Case labels, e.g.
``"happiness"`` → ``"Happiness"`` — matching FER+'s own capitalization
convention while keeping each backend's label vocabulary independent):

.. math::

   \text{label} = \arg\max_c \; \text{score}_c, \qquad
   \text{score}_c \in [0, 1]

:func:`~expression.pyfeat._fex_row_to_result` is the pure, directly
unit-tested piece — isolated from the real ``Detectorv1.detect()`` call
exactly like FER+'s :func:`~expression.ferplus._softmax_and_label` isolates
softmax+argmax from its ONNX session call. Any ``NaN`` value py-feat itself
reports (its own per-row failure marker, e.g. when an internal model
can't produce a score for a marginal detection) is treated as ``0.0``
rather than propagating into the argmax or crashing — the same
default-missing-data-to-zero convention already used for blendshape
name/score lookups on the detection side.

.. note::

   py-feat's own internal face detector re-runs against an already-tight
   MediaPipe-detected crop and can, rarely, fail to find a face in it —
   this backend returns ``("Neutral", 0.0, {})`` in that case (no AU
   values at all) rather than raising and aborting the whole analysis run,
   matching this plugin's established fail-soft precedent for a
   degenerate detection.

Practical recommendations
------------------------------

.. grid:: 1 1 2 2
   :gutter: 2

   .. grid-item-card:: ⚙️  Which backend to pick

      **Heuristic** (default) is transparent, has zero extra download, and
      is fast enough to never be the bottleneck — a reasonable default for
      exploratory analysis. **FER+** trades some transparency for
      generally better accuracy and adds "Contempt" as an 8th category.
      **py-feat** is the most detailed (real FACS Action Units, not just a
      single label) but meaningfully slower (~0.1–0.8 s/frame on CPU) and
      pulls in the heaviest dependency chain of the three (torch +
      torchcodec) — pick it specifically when AU-level output is the
      actual goal, not as a default.

   .. grid-item-card:: 🔬  Reach for py-feat's AUs, not just the label

      If the research question is about *which facial muscles* moved, not
      just "what emotion," py-feat's per-AU CSV export is the more
      scientifically meaningful output — a single dominant-emotion label
      (from any of the 3 backends) discards exactly the compositional
      information FACS coding exists to capture.

   .. grid-item-card:: 🖥️  py-feat's FFmpeg/torchcodec gotcha

      ``import feat`` unconditionally pulls in torchcodec, which needs a
      **torchcodec-compatible FFmpeg (versions 4–8, a shared/DLL build)**
      discoverable on ``PATH`` at runtime — even though this backend never
      touches video I/O directly. If the py-feat backend fails to
      construct, ``where ffmpeg`` (or ``which ffmpeg``) is the first thing
      to check, not a code issue.

   .. grid-item-card:: 🧑‍🤝‍🧑  Subject identity, again

      All three backends inherit the same limitation as every other
      per-frame plugin in this app: no subject identity is tracked across
      frames. Multi-subject sessions are only reliably interpretable if
      you cross-reference against another signal (e.g. bounding-box
      position) to disambiguate who's who frame to frame.
