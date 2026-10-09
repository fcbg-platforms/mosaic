Eye Contact
===========

.. contents:: On this page
   :local:
   :depth: 1

The Eye Contact plugin (``analysis/run_eye_contact.py``) measures where the
face on a camera looks:

- how much of the time it looks at its conversation partner;
- when, for how long and in which direction it looks away;
- how this lines up with speaking, listening and turns.

It is built for interview mode, where the camera faces the interviewee and
the interviewer sits off camera. The rules live in ``analysis/eye_contact/``
(numpy only, unit-tested).

Gaze per frame
--------------

The 3D gaze plugin's geometric eyeball model (:doc:`gaze_fusion`) runs on
the one camera:

- head pose places each eyeball centre;
- the iris pixel is back-projected onto the iris sphere;
- centre-to-iris, plus the kappa angle, is the eye's visual axis.

The two eyes count equally; an eye much less reliable than the other
(closed, or hidden behind the nose) is left out. A blink gives no gaze for
that frame. The direction is smoothed with a centred 100 ms mean.

Angles are in the **camera frame**:

- **yaw** is positive towards image right, which is the subject's left;
- **pitch** is positive up;
- ``(0, 0)`` points straight along the camera's axis, towards it.

Without an intrinsic calibration a nominal lens (focal length = image width)
is used; that tilts every angle a little, which the next step cancels.

Where the partner is
--------------------

**Found from the gaze** (the default). Listeners look at whoever is speaking
much of the time (Kendon, 1967; Argyle and Cook, 1976), so while the subject
listens their gaze clusters on the interviewer. The partner's direction is
the **mode** of the gaze angles:

1. Take the peak of a 2D histogram (1° bins, smoothed with a 2° Gaussian).
2. Refine it by mean shift: average the points within 5° of the peak, and
   repeat until it settles.

The mode is used, not the mean, because looking away pulls the mean towards
wherever the subject looks away to.

With Conversation Timing's output (``conversation/video_N.conversation.json``)
and at least 20 s of listening, only listening frames are used. Otherwise all
frames are used, and the log says so.

Because the partner's direction comes from the same measurements as the
gaze, a constant bias of the gaze estimate cancels:

- kappa varies by a few degrees between people;
- an uncalibrated lens tilts all angles a little;
- the 3D gaze method has shown a pitch bias on real data.

A bias moves both the cluster and the gaze, so the angle between them is
unaffected.

**What this assumes.** The place the subject looks at most while listening
is the partner. When nobody else is in the room, or the subject reads notes
while listening, the "partner" is whatever they looked at most. The log
warns when under 25% of the gaze falls near the direction found. The
annotated video's gaze map shows where it is.

**The camera.** For a remote interview (the partner is on a screen beside or
behind the camera), or an interviewer sitting right next to the camera,
choose the camera as the partner. The partner's direction is then, frame by
frame, the direction from the subject's eyes to the camera. This is an
absolute measure: a bias of the gaze estimate is **not** cancelled, so a
pitch bias of 10° can turn steady eye contact into "looking away". Prefer
the automatic partner when the subject listens for a while.

**By hand**: ``--target manual --target-yaw Y --target-pitch P``.

With the camera or a given direction the cone is fixed at 8° (or
``--radius``). The spread around a direction that was not found from the
gaze would include the gaze's own bias, so it is not used.

The contact cone
----------------

Eye contact is gaze within a **cone** around the partner's direction. Its
radius is

.. math::

   r = \min\big(15°,\ \max(5°,\ 2.5\,\sigma)\big),

where :math:`\sigma` is the spread of the gaze around the partner. It is
estimated robustly from the points within 10° of the partner: their median
distance divided by 1.177 (for a 2D Gaussian, the median distance from the
centre is 1.177 σ). A cone of 2.5 σ holds about 96% of the gaze that is on
the partner. That spread combines measurement noise with the size of a face
at conversation distance (about 7 to 10° across).

Camera gaze cannot tell looking into someone's eyes from looking at their
face, so this measures **looking at the partner's face**. That is the usual
meaning in interview research.

Per frame, the state is contact, away, or unknown. Three clean-up rules
apply, in this order:

- **Returns shorter than 0.15 s** inside a look-away do not end it.
- **Look-aways shorter than 0.3 s** next to contact count as contact
  (saccades and noise).
- **Gaps up to 0.5 s** (blinks, a briefly lost face) between two equal
  states take that state. Gaps are filled last because the iris jumps as
  the eyelid closes, leaving a brief "away" beside many blinks.

Look-aways
----------

Each look-away (gaze aversion) of 0.3 s or more is reported with:

- its start, end and duration;
- its mean angle from the partner;
- its **direction**: ``up``, ``down``, ``left`` or ``right``, by whichever of
  the yaw and pitch offsets is larger. Left and right are the subject's own.

Looking away goes with thinking, and looking up and away with retrieving
information from memory (Glenberg, Schroeder and Robertson, 1998;
Doherty-Sneddon and Phelps, 2005).

With speaking and listening
---------------------------

With Conversation Timing's output, eye contact is also given separately for
three states:

- **speaking**: the subject's speech;
- **listening**: the other's speech while the subject is silent;
- **silence**: neither, within the stretch Conversation Timing analysed
  (where the audio and the video overlap).

Listeners usually look at the speaker more than speakers look at the
listener (Kendon, 1967).

Three patterns are measured over the subject's turns of at least 2 s:

- **Turn start aversion**: the share of turns with a look-away in their
  first 2 s. Speakers tend to look away as they begin.
- **Turn end contact**: the share of turns with eye contact for most of
  their last second, out of the turns whose last second has a measured gaze
  (``turn_ends_measured``). Speakers tend to look back to hand over the turn (Ho,
  Foulsham and Kingstone, 2015).
- **Gap aversion**: the share of response gaps (the silence before the
  subject answers) with a look-away.

Outputs
-------

Per camera, in ``<session>/eye_contact/``:

- ``video_N.eye_contact.json`` (schema ``mosaic-eye-contact-v1``):

  - the partner's direction, how it was found, the cone radius, the spread,
    and the share of gaze inside the cone;
  - the summary and every look-away;
  - per frame: the gaze angles, the angle from the partner and the contact
    state.
- ``video_N.eye_contact.csv`` (per frame, with speaking or listening) and
  ``video_N.aversions.csv``.
- ``video_N.eye_contact.mp4``: the video with:

  - the gaze arrow, green for contact and amber when looking away;
  - the state in large type, with the look-away's direction;
  - a gaze map: the partner at the centre, the contact cone, and the last
    second of gaze. The subject's left is drawn on the right, as the camera
    sees it.

Limits
------

- **One face per camera**: the largest.
- **The eyes must be visible**: gaze needs both the iris and the eyelids.
  Small faces in room recordings (under about 100 px) give noisy gaze.
- **Not tested on a real interview yet.** The rules are tested on synthetic
  gaze. The eye model is the 3D gaze plugin's, with its known upward bias on
  ses-7. The automatic partner cancels that bias; the camera as partner does
  not.
- **The partner is assumed to stay put** relative to the camera. Turning to
  a second interviewer splits the gaze between two clusters; only the
  densest one is the partner.

References
----------

- Kendon, A. (1967). Some functions of gaze-direction in social interaction.
  *Acta Psychologica* 26, 22 to 63.
- Argyle, M. and Cook, M. (1976). *Gaze and Mutual Gaze*. Cambridge
  University Press.
- Glenberg, A. M., Schroeder, J. L. and Robertson, D. A. (1998). Averting the
  gaze disengages the environment and facilitates remembering. *Memory and
  Cognition* 26(4), 651 to 658.
- Doherty-Sneddon, G. and Phelps, F. G. (2005). Gaze aversion: a response to
  cognitive or social difficulty? *Memory and Cognition* 33(4), 727 to 733.
- Ho, S., Foulsham, T. and Kingstone, A. (2015). Speaking and listening with
  the eyes: gaze signaling during dyadic interactions. *PLoS ONE* 10(8),
  e0136905.
