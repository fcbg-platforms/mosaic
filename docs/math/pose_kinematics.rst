Pose Kinematics
==================

.. contents:: On this page
   :local:
   :depth: 2

Implemented in C++ (:cpp:func:`mosaic::compute_kinematics`,
``src/analysis/pose_kinematics.hpp/.cpp``) — not in
:mod:`pose.human_pose`, which only produces the raw keypoint detections
this module derives Speed/Acceleration from. Used by the Analysis tab's
Pose plugin (:doc:`/user_guide`) to plot a keypoint's derived motion
alongside its raw position.

Gap-tolerant finite differences
------------------------------------

A naive velocity estimate assumes a constant nominal frame rate. But real
detections have gaps: a keypoint can be temporarily occluded or
low-confidence for several frames, and simply skipping those samples
(rather than interpolating a fabricated path through them) means the time
between two *valid* consecutive samples isn't always one nominal frame
period — it can span a real gap. Using each sample's own real timestamp
handles this correctly:

.. math::

   v_i = \frac{\lVert \tilde p_i - \tilde p_{i-1} \rVert}{\Delta t_i},
   \qquad
   \Delta t_i = t_i - t_{i-1}

.. math::

   a_i = \frac{v_i - v_{i-1}}{\Delta t_i}

where :math:`\tilde p_i` is the (optionally smoothed — see below)
position at valid sample :math:`i`, and :math:`a_i` uses the *same*
:math:`\Delta t_i` as the speed sample it's paired with. A keypoint below
the visibility threshold, or a frame with no detected subject at all, is
excluded from the sample sequence entirely rather than interpolated.

Centered moving-average smoothing
--------------------------------------

Raw frame-to-frame keypoint jitter makes the *acceleration* estimate
above particularly noisy (a second derivative amplifies noise more than a
first derivative does). An optional centered moving average smooths
**positions** before the finite differences above are computed:

.. math::

   \tilde p_i = \frac{1}{2h+1} \sum_{k=-h}^{h} p_{i+k}

using a window half-width :math:`h`, clamped at the sequence's edges
(no padding — an edge sample averages over however many neighbours
actually exist). Centered (not trailing) is deliberately used since this
is offline analysis over an already-fully-recorded trajectory — there's
no reason to accept a causal filter's phase lag when the whole trajectory
is already available.

Why average speed is time-weighted
----------------------------------------

The reported average speed is **not** the arithmetic mean of the
per-interval speed samples :math:`v_i` above. It's the total path length
over the total elapsed time:

.. math::

   \bar v = \frac{\sum_i \lVert p_i - p_{i-1} \rVert_{\text{unsmoothed}}}{t_N - t_1}

using the *unsmoothed* positions for distance (smoothing is meant to
stabilize the derivative estimate, not misreport the actual path length
travelled).

.. note::

   A naive mean of :math:`v_i` samples silently over- or under-weights
   long gaps: since each :math:`v_i` already divides by its own
   :math:`\Delta t_i`, an unweighted mean of speeds treats a
   1-second-long low-visibility gap's single speed sample exactly the
   same as a 40ms interval's speed sample — even though the gap-spanning
   sample represents 25× more real time. This project's own code review
   caught exactly this discrepancy (an unweighted mean understated true
   average speed whenever a low-visibility gap was present) — the
   time-weighted formula above is the fix, and is the reason gap-tolerant
   sampling (above) matters for more than just correctness of the
   instantaneous samples.

Practical recommendations
------------------------------

.. grid:: 1 1 2 2
   :gutter: 2

   .. grid-item-card:: 🎚️  Smoothing is off by default — and that's deliberate

      Raising **Smoothing** (e.g. to 5) meaningfully stabilizes a noisy
      Acceleration curve at the cost of blurring genuinely fast
      transients. Leave it at 1 (off) first and only raise it once you've
      confirmed the raw signal actually needs it — don't smooth by
      default just because it looks cleaner.

   .. grid-item-card:: 📏  Set Scale honestly, or leave it at 1.0

      Without a real calibration linking pixels to physical distance, the
      **Scale (mm/px)** field is exactly as accurate as the value you
      type into it — a wrong or guessed scale produces a confidently
      wrong physical speed. Leave it at ``1.0`` (px units) unless you
      have an actual measured reference distance in the frame to compute
      the real ratio from.

   .. grid-item-card:: 🕳️  A long low-visibility gap still reports honestly...

      ...but "honestly" doesn't mean "meaningfully." A single reported
      average speed spanning a multi-second occlusion gap is
      mathematically correct (see the time-weighted formula above) but
      may not reflect anything a reader would call the subject's "real"
      average speed during that gap — treat any large gap in the
      exported CSV's timestamps as a flag to sanity-check the surrounding
      numbers, not just trust them.

   .. grid-item-card:: 🧑‍🤝‍🧑  Identity is tracked, but not guaranteed

      Kinematics are computed against a subject *id* — a BoT-SORT track, not
      a position in each frame's detection list — so two people swapping
      detection order no longer corrupts a trajectory. A frame in which that
      person was not detected is skipped exactly like a low-visibility one,
      using the real elapsed time across the gap.

      What this does not promise: an occlusion longer than the tracker's
      buffer ends the track, and the same person resumes under a new id, so
      one physical individual can span several subjects. Each stats line
      therefore reports the time span it covers — check it before reading a
      total distance or an average speed as describing a whole recording.
      Ids restart per video, so they are never comparable across cameras.
