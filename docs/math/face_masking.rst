Face Masking
===============

.. contents:: On this page
   :local:
   :depth: 2

Implemented in :mod:`facemask.masking` — see :doc:`/analysis_api` for the
full API reference. The three detection backends themselves
(:mod:`facemask.detectors`) are pretrained models producing plain
``(x1, y1, x2, y2)`` boxes; this page covers the geometry applied to those
boxes afterward.

Box padding and clamping
-----------------------------

:func:`~facemask.masking.expand_and_clip` pads a detected box by a
fraction of its own size (so hairline, ears, and chin are covered, not
just the tight detector box), then clips the result to the frame bounds.
For a box :math:`(x_1, y_1, x_2, y_2)` with width :math:`w = x_2-x_1` and
height :math:`h = y_2-y_1`, and a margin fraction :math:`m`:

.. math::

   x_1' = \max(0,\; x_1 - m w), \qquad
   x_2' = \min(W,\; x_2 + m w)

.. math::

   y_1' = \max(0,\; y_1 - m h), \qquad
   y_2' = \min(H,\; y_2 + m h)

where :math:`W, H` are the frame's width/height. Padding is applied
*before* clipping, so a face near the frame edge is padded first and only
then clamped — the result is always a valid, non-negative box even at the
frame boundary, never an out-of-bounds region a caller could mis-slice.

Blur-kernel sizing
-----------------------

:func:`~facemask.masking.apply_mask`'s ``"blur"`` style applies a Gaussian
blur whose kernel size scales with the box's own size, so small/distant
and large/close faces both get proportionally strong blur:

.. math::

   k = \max\!\big(3,\; \lfloor \min(w, h) / 3 \rfloor \;\vert\; 1\big)

where :math:`\vert` is bitwise OR, used here purely to force the result
odd (OpenCV's Gaussian blur requires an odd kernel size), and the
:math:`\max(3, \ldots)` floor guarantees a valid minimum kernel even for a
very small box. The computed kernel is additionally clamped to the box's
own actual width/height (never larger than the region being blurred) — a
region too small for any valid odd kernel falls back to a solid fill
instead, so a detected face is never left completely unmasked purely
because ``cv2.GaussianBlur`` rejected an oversized kernel for a thin,
edge-clipped box.

Practical recommendations
------------------------------

.. grid:: 1 1 2 2
   :gutter: 2

   .. grid-item-card:: ⚙️  Which detection backend to pick

      **MediaPipe** (default) has the best recall across angles. **YOLOv8**
      is a community checkpoint — verify it's finding faces you'd expect
      before trusting it for a privacy-critical run. **OpenCV DNN** avoids
      an extra ML framework but is noticeably weaker at extreme angles —
      only use it when the other two aren't viable options.

   .. grid-item-card:: 🔒  Privacy is only as strong as recall

      This tool's entire purpose is anonymization — a **missed** detection
      means an unmasked face in the output, silently. Spot-check the
      anonymized output on a few frames per camera before treating it as
      safe to share externally, especially for a backend/session
      combination you haven't validated before.

   .. grid-item-card:: 🎞️  Keep frame-skip low for anonymization

      Unlike Pose/Expression's frame-skip (which only thins out *recorded*
      data), Face Masking's skip control reuses the last detected box on
      skipped frames — a fast-moving face between detected frames can
      leave a real gap in coverage. Keep it at 1 (every frame) unless
      you've confirmed the subject's motion is slow enough for a higher
      value to still cover every frame adequately.

   .. grid-item-card:: 🫥  Blur vs. solid box

      Blur is visually softer and preserves general context (hair,
      approximate shape); a solid box is a harder, more obviously
      irreversible guarantee against any residual leakage through a
      partially-transparent or thin blur kernel — prefer solid box when
      the anonymization requirement is strict, not just visual.
