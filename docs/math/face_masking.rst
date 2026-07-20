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
very small box.
