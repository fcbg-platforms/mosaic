Hands
=====

.. contents:: On this page
   :local:
   :depth: 1

Implemented in ``analysis/run_hands.py`` and the ``hands`` package
(``models.py``, ``people.py``); shown in the Analysis tab's **Hands** entry.
Made for close-up cameras on a table (room 11's Cameras 1 and 5), where a
hand is large enough to see the fingers. This is the first step of the hand
analyses: the points per frame. Speeds, grip aperture, reaches and grasps
come next and read this result.

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

Limits
------

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
  0.5 s, or 2.5 analysed frames when longer), median size in pixels and mean
  score;
- ``video_N.hands.frames.json``: every analysed frame's hands, each with
  its person, side, score, 21 points (0.1 pixel) and their scores;
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
