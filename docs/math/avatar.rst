3D Avatar
=========

.. contents:: On this page
   :local:
   :depth: 1

Implemented in ``analysis/run_avatar.py`` and the ``avatar`` package
(``people.py``, ``fit.py``, ``io.py``); run from the Analysis tab's **3D
Avatar** entry and listed in the session report. It puts a realistic body
on each person of the 3D Pose result: its shape once (age, height, build)
and its pose at every tick. The 3D Pose room view and the 3D Gaze view
draw them (see
`Drawing`_).

The body model
--------------

**Anny** (NAVER LABS Europe; Brégier et al., ECCV 2026) is a differentiable
human body model in PyTorch, built on the MakeHuman community's body. One
model covers all ages, from infants to the elderly: its shape is six
interpretable parameters, each 0 to 1 (gender, age, muscle, weight, height,
proportions), which its authors calibrate on WHO population statistics. A
child gets a child's proportions, not a scaled-down adult's, which matters
for MOSAIC's studies with children. Its skeleton has 104 bones, fingers and
toes included; its mesh 13,718 vertices and 27,420 triangles, each vertex
moved by up to 9 bones (linear blend skinning, with no pose-dependent
corrections). The code is Apache-2.0 and the MakeHuman assets it uses are
CC0, so MOSAIC installs it with the analysis environment. Anny also gives
the 17 COCO keypoints from its mesh (a fixed weighted sum of vertices per
keypoint), the same points 3D Pose finds; the fit matches the two.

Anny's pose parameters are changes from its **reference pose**, a standing
pose with the arms down and the elbows already bent (about 36 degrees),
not from the mesh's rest shape. A bone at zero is in its reference pose.

The frame
---------

3D Pose's room frame is the reference camera's (y points down). The fit
works in metres in a frame with z up: up is the mean direction from the
ankles to the shoulders over the recording (from the hips to the shoulders
when no ankle is ever seen, everyone behind a desk; the room's -y when
neither is), x the room's x made level. ``to_room`` in ``avatar.json`` takes
the fit's frame back to the room's millimetres.

The fit
-------

**Which ticks.** A tick can start a fit when 3D Pose found at least 8 of
its 17 joints there, both hips and both shoulders among them (the hips say
where the body is and which way it faces). A person with no such tick is
not fitted, and listed under ``failed``.

**Stage 1, the shape.** On 60 such ticks spread over the recording, the
shape, every sampled tick's pose, and an offset of each COCO hip point are
fitted together. COCO's hip points (where a detector trained on COCO images
puts the hips) sit higher and wider than the hip joints, a steady difference
rather than noise: a fixed offset per side, turning with the pelvis and at
most 15 cm, takes it up, rather than the body bending to chase it. Each
tick starts facing the way its hips say, with the body's hips on 3D Pose's
and the limbs nearly straight. A weak pull towards an average body (2e-5
times the squared shape parameters, before their sigmoid) keeps the shape
from absorbing the joints' noise without pulling a child towards an adult.

**Stage 2, every tick's pose,** with the shape fixed, each tick starting
from the sampled tick nearest in time, in pieces of 600 ticks that overlap
by 40 (each keeps its middle). The pose is the root's rotation and
position, and the rotations of 22 body bones: the legs' upper parts and
feet, five spine bones, the clavicles, shoulders, upper arms and wrists,
two neck bones and the head. The other bones (fingers, toes, eyes) stay in
the reference pose.

**Hinges.** Knees and elbows have one bend angle each, about one axis, so
they never bend backwards or sideways. The axis is the one, among the bone
frame's three axes (both ways) and the normal of the limb's own plane in
the reference pose, whose rotation brings the far end (ankle, wrist)
towards the near one (hip, shoulder) and keeps it in that plane; for a limb
nearly straight in the reference pose (the knees), which has no plane yet,
the one bending it backwards for a knee, forwards for an elbow. The bend
runs from the limb straight to 150 degrees in all: measured from the
reference pose, so from minus the reference bend (an elbow can straighten)
to 150 degrees minus it.

**The loss**, per tick: the distance from each of the body's COCO
keypoints to 3D Pose's, robust (a Huber loss, quadratic up to 5 cm and
linear beyond, so one bad joint does not pull the body), averaged over the
joints 3D Pose found; the face points (nose, eyes, ears) count 0.3, being
less sure and not exactly where Anny's are. Plus a weak pull of each bone
towards its reference pose, and smoothness: the squared second difference
over time of the rotations (averaged over the bones, weight 2), the hinge
angles (weight 2) and the position (weight 10, in metres), only across
ticks at most 2 apart, never across a person's absence. These weights
matter: the data term is a mean over the joints, the smoothness per tick,
and ten times stronger smoothness (an earlier version) left the hips a
median 7 cm and in a fifth of the ticks over 20 cm from 3D Pose's while
following everything else. Adam, 600 steps for the shape and 1,500 for the
poses.

**Speed.** With the shape fixed, each keypoint is a fixed combination of
the bones' skinning transforms (the keypoints are fixed sums of vertices,
and the vertices fixed sums of bone transforms), computed once; the fit then
needs only the skeleton's transforms at each step, never the mesh. On an
NVIDIA RTX A4000, one person over 31 s of recording takes about 90 s: two
to three minutes per minute of recording per person; on a CPU several times
longer.

How well it fits
----------------

``avatar.json`` gives, per person, the median and 90th percentile distance
from the body's keypoints to 3D Pose's, over all joints and per joint (the
median alone hides a body that is far off now and then). On sub-39 (one
person walking about the room, sitting at and leaning over the desk; 3D
Pose from Cameras 2 to 4):

.. list-table::
   :header-rows: 1
   :widths: 40 30 30

   * - Joints
     - Median
     - 90th percentile
   * - Nose, eyes
     - 10 to 16 mm
     - 19 to 26 mm
   * - Ears
     - 19 to 21 mm
     - 32 to 34 mm
   * - Shoulders, elbows, wrists
     - 16 to 20 mm
     - 40 to 56 mm
   * - Hips (with the learnt offset)
     - 25 mm
     - 60 to 66 mm
   * - Knees
     - 39 mm
     - 105 to 115 mm
   * - Ankles
     - 60 to 64 mm
     - 155 to 210 mm
   * - All
     - 19 mm
     - 58 mm

The body moves more steadily than 3D Pose's joints: their median second
difference from tick to tick is 2 to 10 mm for the body's joints against
10 to 54 mm for 3D Pose's arms and legs (about the same for the head). Its
elbows straighten as 3D Pose's do (10th percentile of the bend 14 and 22
degrees against 21; median 63 to 64 against 65).

Drawing
-------

The 3D Pose room view and the 3D Gaze view (``src/analysis/avatar_body.cpp``,
``src/ui/analysis/room3d_scene.cpp``) each read each person's file when the
view is opened for the session, and again only when a file has changed: about 45 MB
per person per 10 minutes of recording, most of it the bone transforms. At each frame it takes the person's tick nearest
in time (none further than 100 ms) and skins the rest mesh with its bone
transforms, the same linear blend skinning as Anny's, on the CPU. The fit's
frame, the room's and the view's are composed into one matrix per bone
first. Normals are worked out per vertex from the posed triangles, weighted
by their area. Neither frame change mirrors, so the triangles keep Anny's
outward winding.

The body is matt, like clay, in a soft tone of the person's colour: it
shows posture and movement, not a look. Each vertex belongs to the region
of the bone that moves it most; a lower leg (with the foot) or a forearm
(with the hand) is drawn paler at a tick when 3D Pose did not measure both
of its joints. Drawn as a body, a person's bones, joints, head and torso
are left out, unless **Avatar + skeleton** is chosen.

When ``skeleton3d.json`` is newer than ``avatar/avatar.json``, the bodies
are not drawn: they were fitted to an earlier 3D Pose result, whose ticks
and people may not match.

Limits
------

- **What 3D Pose does not see.** Its 17 joints say nothing of the spine's
  twist, the hands' and feet's rotation, or the fingers: these stay near
  the reference pose or follow the priors, so the avatar can look calmer
  than the person. The fingers can come from 3D hands, the head's turn from
  3D Gaze (later versions).
- **Hidden legs.** Legs behind or under a desk are measured little or not
  at all: the ankles' 90th percentile above is mostly those. The hinges and
  smoothness keep them plausible, not right, and the feet can cross (in 5%
  of sub-39's ticks, never in 3D Pose's). Each tick records which joints 3D
  Pose measured, and the room view draws the rest paler.
- **One shape per person.** A person 3D Pose splits into two people gets
  two bodies.
- **Clothing.** The body is unclothed and without a face texture: an avatar
  shows a person's posture and movement, not who they are, so it can be
  shown where the video cannot.

Output
------

In ``<session>/avatar/``:

- ``avatar.json`` (schema ``mosaic-avatar-v1``): the body model, the 17
  keypoint names, ``to_room`` (from the fit's frame, metres with z up, to
  the room's millimetres), per person its id, file, the counts in it, its
  shape, its hip offsets (mm), how long it was seen, and its median and
  90th percentile distances to 3D Pose's joints, and ``failed``, the people
  that could not be fitted. ``--person`` fits some people again and keeps
  the others; a full run starts over;
- ``person_N.bin``: in this order, little-endian: the body at rest (float32
  vertices x 3, metres), its triangles (uint32 x 3), its skinning (uint16
  bones and float32 weights, 9 per vertex), every tick's time (float64 ms)
  and which 3D Pose joints were measured (a uint32 bit per joint), and every
  tick's bone transforms (float32: a unit quaternion x, y, z, w and a
  translation in metres, per bone); a vertex is the weighted sum of its
  bones' transforms applied to it. About 3 KB per person per tick (some
  45 MB for ten minutes at 25 ticks a second).

References
----------

- Brégier, R., Fiche, G., Bravo-Sánchez, L., Lucas, T., Armando, M.,
  Weinzaepfel, P., Rogez, G., and Baradel, F. (2026). Human Mesh Modeling
  for Anny Body. In Computer Vision, ECCV 2026, pp. 170-188. Springer.
  arXiv:2511.03589. Code: https://github.com/naver/anny (Apache-2.0).
- MakeHuman community assets (CC0): https://static.makehumancommunity.org/
- Huber, P. J. (1964). Robust estimation of a location parameter. The
  Annals of Mathematical Statistics, 35(1), 73-101.
- Kingma, D. P., and Ba, J. (2015). Adam: a method for stochastic
  optimization. ICLR.
