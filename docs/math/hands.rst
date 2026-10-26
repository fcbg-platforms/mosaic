Hands
=====

.. contents:: On this page
   :local:
   :depth: 1

Implemented in ``analysis/run_hands.py`` and the ``hands`` package
(``models.py``, ``people.py``, ``kinematics.py``, ``movements.py``);
shown in the Analysis
tab's **Hands** entry and in the session report. Made for close-up cameras
on a table (room 11's Cameras 1 and 5), where a hand is large enough to see
the fingers. It finds the points per frame, then works out the hand's
kinematics from them: speeds, grip aperture, hand opening and finger bend.
Last it finds each hand's movements (reaches, grasps, any move) with the
measures the reaching and grasping literature uses, and how each person
used their two hands.

What is found
-------------

For each hand in each analysed frame: **21 points** in pixels (the wrist,
then each finger's joints out to its tip: thumb CMC, MCP, IP and tip; for
each other finger MCP, PIP, DIP and tip), a **score** per point and for the
hand, **whose hand** (the Pose subject id) and **which side**, the person's
own left or right. The 21 points are in the same order for both models
(MediaPipe's, which COCO-WholeBody's hands share).

Where the people are comes from the camera's **Pose** result, so Pose runs
first on the same camera. Pose may analyse every Nth frame; a frame between
two Pose frames takes the nearer one (as in Object Tracking).

The two models
--------------

**RTMW + hand model** (the default). RTMW (Jiang et al., 2024) is a
whole-body model, used here as RTMW-l distilled from RTMW-x, at 384 x 288:
on each person's Pose box (one box per person: of two overlapping by more
than half, the more confident) it gives 133 points, 21 of them for each
hand, labelled with the person's own side. A whole-body model sees
the hand small, about 40 pixels across in its 288 x 384 input, so each hand
is then looked at again, close up: the box of RTMW's hand points, widened
1.6 times, goes to the RTMPose hand model (RTMPose-m, trained on five hand
datasets, 256 x 256 input). Its points are kept when they lie on the hand
RTMW found (their centre within 30% of the hand's size of RTMW's); with the
hands together the crop holds both, and it may pick the other one, so then
RTMW's own points are kept. A hand is kept when its points score at least
0.3 on average, at both steps. Both models are MMPose's (OpenMMLab,
Apache-2.0), exported to ONNX and run by onnxruntime;
the crop and the decoding of their output (SimCC: a score per half input
pixel along each axis, the point at the highest) follow rtmlib (Apache-2.0).

**MediaPipe** (Google's Hand Landmarker, Apache-2.0). Its own palm detector
looks at the whole frame shrunk to 192 pixels, where a hand on a 1920 pixel
frame is about 16 pixels: too small. So it is given a square crop around
each hand instead, centred a little past each wrist Pose sees (35% of the
forearm beyond it), 2.2 times the hand's expected size (80% of the forearm,
120 to 400 pixels), resized to 256 pixels, where it looks for up to two
hands (with the hands together, both are in each wrist's crop). MediaPipe's
own left and right flips from frame to frame on a hand seen from the side,
so it is not used: each hand takes the person and side of the nearest
wrist Pose sees, nearest pairs first, each wrist once, within the hand's
own size (at least 60 pixels). A hand no wrist is near is kept as "unknown". With the elbow not
seen, the crop is sized for a 200 pixel hand. MediaPipe gives no score per
point: a hand is kept when MediaPipe is at least 0.4 sure a hand is there,
and its score is MediaPipe's confidence in its left or right label.

Both models download their files on first use into
``analysis/hands/models/``, each checked against a pinned sha256.

Two hands found on top of each other (two crops on one hand) are kept as
one, the better scored: centres closer than 30% of the larger hand, unless
they are known to be different hands (another side, or another person: a
person's two hands on one ball, or two people's hands meeting, are all
kept).

How the models compare
----------------------

On a room-11 recording (sub-31, an adult turning a ball and toys over at the
table; Cameras 1 and 5, 21 fps, hands 130 to 300 pixels across):

.. list-table::
   :header-rows: 1
   :widths: 40 30 30

   * -
     - MediaPipe (crops)
     - RTMW + hand model
   * - Hands holding the ball
     - collapsed or missing
     - both hands, fingers around it
   * - Frames with a hand (Camera 1 / 5)
     - 54% / 65%
     - 75% / 94%
   * - Frames with both hands (Camera 1 / 5)
     - 9% / 18%
     - 57% / 73%
   * - Left and right
     - flips (not used; from the wrists, a few hands none)
     - the person's own
   * - CPU time per frame
     - 50 to 60 ms
     - 75 to 90 ms

Hands holding an object are the hardest case, and the one a grasping study
needs most; RTMW was trained with many hand-object scenes, MediaPipe's
model mostly with open hands. On the CPU, RTMW takes about twice the
recording's length per camera at every frame.

Kinematics
----------

Each hand (one person, one side) is followed over the recording, on the
frames analysed. Hands of no known person or side are left out: two of them
in a row need not be one hand.

**Cleaning.** A point is not used when:

- the model scored it under 0.2 (fingertips score lower than the wrist: at
  0.3 a fifth to two fifths of them were dropped on sub-31, at 0.2 almost
  none);
- its frame's palm length (wrist to the middle finger's knuckle) is under
  two thirds or over one and a half times the palm over the second around:
  the wrong hand, or a badly placed one. A frame whose wrist or middle
  knuckle is unsure keeps its other points (its palm cannot be checked);
- it is far from its own median over 7 analysed frames: more than 3 scaled
  median absolute deviations and half a palm (a Hampel filter). The models jump to a wrong
  place for a frame or two now and then, and one such jump would read as a
  burst of speed.

**Filling and smoothing.** A hand's frames are split into stretches where
two analysed frames in a row are over 0.2 s apart (or two and a half
analysed frames, when that is longer: at every Nth frame), stretches under
0.5 s are left out, and each stretch is cleaned on its own (above). Within
a stretch each point is put on a regular grid at the analysed frame rate,
filled in a straight line only between two of its own samples at most that
far apart, and never held past its last sample. Each point's unbroken
stretches are then low-pass filtered: a 2nd-order Butterworth at 6 Hz (at
most 0.4 times the frame rate), run forwards and back so nothing is
delayed; a point's stretch of 9 grid samples or fewer is too short to
filter and left out. Speeds and accelerations are then central
differences. Run forwards and back, the filter halves the power at 6 Hz
and passes about 80% at 4 Hz: the frame-to-frame jitter is well above it,
but very fast or back-and-forth movements are read somewhat slow (on a
synthetic 3 to 4 Hz shake, the 95th percentile speed came out 12 to 25%
low).

**Every Nth frame.** Kinematics need at least 10 analysed frames per second
(every frame, or every 2nd at 21 to 25 fps); with fewer the result has no
kinematics, and the log says why.

**Units.** The points are pixels on one camera, so speeds are given in
pixels per second and in **hand lengths per second**: divided by the hand's
own palm length over the second around (its median, so that a palm turned
away for a moment does not change it). This cancels how far the hand is
from the camera; a hand moving along the camera's line of sight is not
seen moving. Grip aperture and hand opening are in palm lengths too, the
usual way to compare people with different hand sizes. For a palm of about
10 cm (an adult's; a young child's is shorter), 1 hand length per second is
about 0.1 m/s. Millimetres need both table cameras: 3D hands.

**Measures**, per analysed frame:

- the wrist's position (pixels) and its speed, in pixels and hand lengths
  per second; the index and thumb tips' speeds and the wrist's acceleration,
  in hand lengths;
- **grip aperture**: thumb tip to index tip, in palm lengths;
- **hand opening**: the fingertips' mean distance from the wrist, in palm
  lengths, when at least three tips are measured;
- **finger bend**, per finger: one minus the distance from its base to its
  tip over the length of its segments: 0 when straight, towards 1 when
  curled. A straight finger stays straight in any view (points on a line
  stay on a line), so 0 is reliable; how bent a bent finger looks depends
  on the view.

**Summary**, per hand: the time measured (filled gaps included, so it
can be a little more than the time the hand was seen), the wrist's path in
hand lengths,
the median and 95th percentile of its speed, and the median and 95th
percentile of grip aperture and the median hand opening. The 95th
percentile, not the maximum, stands for the fast movements: one bad frame
left after cleaning would set the maximum.

On sub-31 (an adult turning a ball over, Camera 5), the hands of the
person seated at the table moved at a median 1.4 to 1.6 hand lengths per
second, 95th percentile 5.6 to 7.6 (about 0.6 to 0.8 m/s), with a grip
aperture of 0.5 to 0.7 palm lengths. The same camera's other, partly seen
hands (the person walking in, under other Pose ids) read 1 to 3 m/s: a
small or partly seen hand gives inflated speeds.

Movements
---------

A **movement** is a stretch where the hand's wrist moves faster than 0.5
hand lengths per second (about 5 cm/s for a 10 cm palm), peaks at 1.5 or
more, and lasts at least 0.2 s (from its first to its last sample
above). A threshold in the hand's own units works the same for near and far
hands; it finds reaches and grasps, and also any other move of the hand, so
which movements are reaches depends on the task. The thresholds are the
module's constants (``hands/movements.py``).

A movement the measurement starts or ends during (the hand appears already
moving, or is lost mid-movement, at a gap or between two of the hand's
runs) is **cut off**: its duration, peak and shape would be wrong, so it is
left out of the measures, and counted. It still counts as that hand moving
for the laterality index and for both hands moving (below). How many are
cut off depends on how continuously the hand is measured: on sub-31 about
half were, for a person often half out of view; a hand in full view at the
table is cut off far less.

Per movement (``video_N.hands.movements.csv``):

- **start and end**, session time, and **duration** (end minus start), over
  the part above 0.5 hand lengths per second;
- **peak speed**, and **time to peak** as a share of the movement: a reach
  aimed at a target speeds up quickly and slows down for longer (peak
  before 50%), a guided or uncertain one has a long slow ending;
- **mean speed** and **path**, in hand lengths, and **straightness**: the
  wrist's straight-line distance from start to end over its path, both in
  pixels (1 is a straight line);
- **movement units**, after von Hofsten's (1991) units for infants'
  reaches (one speeding up and one slowing down): speed peaks at least 0.2
  hand lengths per second (about 20 mm/s) above the lowest speed on each
  side of them before a higher peak (their prominence). 1 for one smooth
  movement, more for a reach made of several corrections;
- **SPARC**, the spectral arc length (Balasubramanian et al., 2015): minus
  the length of the speed profile's normalised magnitude spectrum, from 0 up
  to 10 Hz (or the Nyquist frequency, which the authors' reference code
  does not cap at; it changes nothing at 21 to 25 fps), cut where it falls
  under 5% of its peak; the same as that code otherwise. Closer to 0 is
  smoother: -1.41 for one minimum-jerk reach, at any frame rate and speed;
  a movement with stops and starts is lower (two minimum-jerk reaches in a
  row: under -1.8);
- **maximum grip aperture** (palm lengths) and when it comes, as a share
  of the movement: in a reach to grasp the hand opens on the way, wider
  than the object, and closes on it near the end (at 60 to 80% of the
  movement in adults; Jeannerod, 1984).

Movement units and SPARC are over the movement's whole speed profile,
widened from the 0.5 threshold to the speed's nearest low point on each
side. Cut at the threshold, a slow movement would start and end on a slope,
and its SPARC would read it as less smooth than a fast one of the same
shape (-1.74 against -1.41 for a minimum-jerk reach peaking at 1.5 and at
9.4 hand lengths per second): children's slower reaches would look jerkier
than they are.

Per hand (``video_N.hands.json``, ``movements``): how many movements, and
the medians of the above. Per person, when both hands were measured
(``people``):

- the **laterality index**, (right - left) / (right + left) movements: -1
  all with the left hand, 1 all with the right;
- how long **both hands moved** at once, in seconds and as a share of the
  time either did.

At 21 to 25 fps, a movement's speed profile has about 20 points per
second: peak speed and time to peak are within one frame (40 to 50 ms),
and short sub-movements can merge, so movement units are counted low for
fast, corrected reaches. SPARC needs the whole profile and is the most
sensitive to the frame rate and to the filter (above).

On sub-31 (Camera 5), the seated person, turning a ball over in both
hands, made 3 and 7 complete movements with the left and right hand (7 and
2 more cut off), a median 0.7 to 0.8 s long, with 3 movement units and
SPARC -1.7 to -1.9; both hands moved together about half the time either
did, and the two hands were used about equally (laterality -0.05).

Limits
------

- **Small or partly seen hands.** Their points jump more, and their palm
  length, the unit, is less sure: their speeds read high. Use the hands of
  the people at the table.
- **Frame rate.** At 21 to 25 fps a reach (0.5 to 1 s) is followed, but the
  moment of a grip, finger tapping or a tremor above about 6 Hz is not:
  analyses of speed will say so. **every N frames** above 1 makes this
  worse; keep 1 for speeds and grasps.
- **Children.** Both models learned mostly from adults' hands. Before
  results on children are reported, check the points against a few hundred
  frames marked by hand.
- **Whose hand.** The person is Pose's subject id on that camera; when Pose
  loses and finds a person again, the same person gets a new id. Linking
  them is for the 3D step. A person Pose did not track has no id: their
  hands are "someone's".
- **Model terms.** The models' code and weights are Apache-2.0; they were
  trained on public datasets whose own terms apply. Check them before any
  commercial use.

Output
------

Per camera, in ``<session>/hands/``:

- ``video_N.hands.json`` (schema ``mosaic-hands-v1``): the model, every N
  frames, the video's first frame time, the point names and, per hand
  (person, side): frames seen, time seen, when (runs ending at a gap of
  0.5 s, or 2.5 analysed frames when longer), median size in pixels, mean
  score and its ``kinematics`` (the summary above);
- ``video_N.hands.frames.json``: every analysed frame's hands, each with
  its person, side, score, 21 points (0.1 pixel) and their scores;
- ``video_N.hands.kinematics.csv``: per hand and analysed frame, the
  measures above, one column each (empty where not measured);
- ``video_N.hands.movements.csv``: one row per movement, the measures
  above (empty where not measured); ``video_N.hands.json`` adds each
  hand's ``movements`` summary and the ``people`` list;
- ``video_N.hands.mp4``: with **Annotated video**, the video with each hand
  drawn, the person's left in green and right in orange, labelled "S3
  left"; between analysed frames the last result holds.

References
----------

- Jiang, T., Xie, X., and Li, Y. (2024). RTMW: Real-Time Multi-Person 2D and
  3D Whole-body Pose Estimation. arXiv:2407.08634.
- Jiang, T., Lu, P., Zhang, L., et al. (2023). RTMPose: Real-Time
  Multi-Person Pose Estimation based on MMPose. arXiv:2303.07399.
- Zhang, F., Bazarevsky, V., Vakunov, A., et al. (2020). MediaPipe Hands:
  On-device Real-time Hand Tracking. arXiv:2006.10214.
- rtmlib: https://github.com/Tau-J/rtmlib (Apache-2.0).
- Winter, D. A. (2009). Biomechanics and Motor Control of Human Movement
  (4th ed.). Wiley. (Low-pass filtering of movement data.)
- Balasubramanian, S., Melendez-Calderon, A., Roby-Brami, A., and Burdet,
  E. (2015). On the analysis of movement smoothness. Journal of
  NeuroEngineering and Rehabilitation, 12, 112.
- von Hofsten, C. (1991). Structuring of early reaching movements: a
  longitudinal study. Journal of Motor Behavior, 23(4), 280-292.
- Jeannerod, M. (1984). The timing of natural prehension movements.
  Journal of Motor Behavior, 16(3), 235-254.
- Hampel, F. R. (1974). The influence curve and its role in robust
  estimation. Journal of the American Statistical Association, 69(346),
  383-393.
