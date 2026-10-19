Social Interaction
==================

.. contents:: On this page
   :local:
   :depth: 1

Implemented in ``analysis/run_social.py`` and the ``social`` package
(``signals.py``, ``synchrony.py``, ``mimicry.py``); shown in the Analysis
tab's **Social Interaction** entry and in the session report.

The question
------------

Two people talking tend to coordinate without noticing: they move at the
same moments, one smiles shortly after the other, their heart rates can rise
and fall together. This analysis measures that coupling between two people,
each seen by the camera that faces them (person A and person B, Cameras 2
and 5 by default). It analyses nothing from the video itself: it reads what
Face Dynamics, Pose and Remote Heart Rate wrote for those two cameras.

Every camera's analyses use the session's elapsed clock, so the two people's
signals line up without further syncing.

The signals
-----------

.. list-table::
   :header-rows: 1
   :widths: 22 30 48

   * - Signal
     - From
     - What it is
   * - Head movement
     - Face Dynamics
     - The head's angular speed, degrees per second, per frame; missing
       where no face was found.
   * - Body movement
     - Pose
     - The mean speed of the main person's visible keypoints between
       frames (confidence above 0.5, at least four seen in both frames), in
       body heights per second: pixels divided by the person's box
       height, so near and far people compare. The main person is the
       largest box in view: the one the camera faces.
   * - Heart rate
     - Remote Heart Rate
     - Each window's rate at the window's centre, filled in between windows
       at most 5 s apart; windows with a poor signal (the app's lowest
       quality tier: SNR under -5 dB, or under 60% usable frames) are left
       out.
   * - Smiles, nods
     - Face Dynamics
     - The start of each smile and each nod.

Movement is put on a grid of 10 samples per second (the mean of the frames
in each 0.1 s), heart rate on 1 per second.

Synchrony: windowed cross-correlation
-------------------------------------

Coupling changes over a conversation, so one correlation over the whole
recording would hide it. **Windowed cross-correlation** (WCC; Boker,
Rotondo, Xu and King, 2002) cuts both signals into short windows and, within
each, correlates B's signal with A's at a range of lags:

.. math::

   r_w(\tau) = \operatorname{corr}\big(x[t_w, t_w + W),\; y[t_w + \tau, t_w + \tau + W)\big),
   \qquad |\tau| \le \tau_{\max}

The window's **peak correlation** :math:`\max_\tau r_w(\tau)` and its lag are
kept. A positive lag means B does what A did :math:`\tau` seconds earlier:
**A leads**. A correlation is computed over the samples present in both
signals; a window with less than 70% of them present is left out.

.. list-table::
   :header-rows: 1
   :widths: 25 25 25 25

   * - Signal
     - Window :math:`W`
     - Step
     - Largest lag :math:`\tau_{\max}`
   * - Head, body movement
     - 5 s
     - 0.5 s
     - 3 s
   * - Heart rate
     - 30 s
     - 5 s
     - 10 s

**Against chance.** Peak correlations are well above zero even for two
people who never met, because the peak is the best of many lags, and slow
signals correlate by accident. So the real pair is compared with **surrogate
pairs**: B's whole signal circularly shifted by a random offset, 100 times.
A shift keeps each person's own rhythm and how much they move, but breaks any
timing between them (Moulder, Boker, Ramseyer and Tschacher, 2018). Shifts
are at least one window plus both lags, so a shifted copy cannot line up
with itself.

For each pair (real or surrogate), the mean peak correlation over windows is
taken through Fisher's :math:`z` (averaging :math:`\operatorname{artanh} r`, then
back). Reported:

- **mean peak r** of the real pair and of the surrogates;
- **synchrony**: the real minus the surrogates' mean;
- **p**: the share of surrogates at least as high as the real pair (with
  the usual +1 in numerator and denominator), one-sided;
- **who leads**: among the windows whose peak beats the surrogates' 95th
  percentile of single-window peaks (the moments clearly coupled), the share
  where A leads and where B leads, and their median lag.

The chart in the Analysis tab shows each window's peak correlation with that
95th percentile as a line: windows above it are coupled more than chance
gives.

A measure needs the two people to be measured together for at least twice
(window + both lags) plus 1 s: 23 s for movement, 101 s for heart rate.
Shorter recordings say so instead of giving a number without a chance level.

Mimicry
-------

For each of A's smiles, B **follows** if B starts a smile within the next 3 s
(strictly after A's start)
(a common window for facial mimicry; Hess and Fischer, 2013); the rate is the
share of A's smiles that B followed, and likewise for nods and for A
following B. A's events in the last 3 s are not counted, since B had no
chance to follow.

Two people who both smile often follow each other by coincidence, so the
same rate is computed with B's smile times circularly shifted (200
surrogates, by at least 6 s): the **chance rate**. **Mimicry** is the real
rate minus the chance rate, and **p** the share of surrogates at least as
high. The two people must share more than 12 s with a face (four times the
3 s window), else no rate is given.

Limits
------

- **Correlation is not causation.** Coupling can come from a shared cause:
  both people react to the same noise, or the interviewer's questions pace
  both.
- **The cameras must face the people.** A camera that sees a person from
  behind, or only at the picture's edge, gives no usable movement; the
  analysis says so (a signal present less than 20% of the time) and names
  the camera.
- **Head speed and smiles come from one camera each**: a person turning
  away from their camera reads as no face, not as no movement.
- **Remote heart rate is experimental** (see :doc:`remote_heart_rate`), and
  heart-rate synchrony is on top of it; read it as exploratory.
- **Interview recordings** show only one person, so there is no pair: the
  analysis refuses them.
- Not yet included: mutual gaze and joint attention (they need the room
  calibration and 3D gaze, :doc:`gaze_fusion`), and voice synchrony.

References
----------

- Boker, S. M., Rotondo, J. L., Xu, M., and King, K. (2002). Windowed
  cross-correlation and peak picking for the analysis of variability in the
  association between behavioral time series. *Psychological Methods*, 7(3),
  338-355.
- Moulder, R. G., Boker, S. M., Ramseyer, F., and Tschacher, W. (2018).
  Determining synchrony between behavioral time series: an application of
  surrogate data generation for establishing falsifiable null-hypotheses.
  *Psychological Methods*, 23(4), 757-773.
- Hess, U., and Fischer, A. (2013). Emotional mimicry as social regulation.
  *Personality and Social Psychology Review*, 17(2), 142-157.
