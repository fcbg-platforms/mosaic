Hands
=====

.. contents:: On this page
   :local:
   :depth: 1

Implemented in ``analysis/run_hands.py`` and the ``hands`` package
(``models.py``, ``people.py``, ``kinematics.py``); shown in the Analysis
tab's **Hands** entry and in the session report. Made for close-up cameras
on a table (room 11's Cameras 1 and 5), where a hand is large enough to see
the fingers. It finds the points per frame, then works out the hand's
kinematics from them: speeds, grip aperture, hand opening and finger bend.
Reaches and grasps come next and read these.

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
- Hampel, F. R. (1974). The influence curve and its role in robust
  estimation. Journal of the American Statistical Association, 69(346),
  383-393.
