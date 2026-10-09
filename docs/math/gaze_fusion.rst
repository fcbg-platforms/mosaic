Multi-camera 3D gaze
====================

Where every subject in the room looks, in 3D: whose face, which named
region, which point on the calibrated plane, or simply which point in space.
Implemented in ``analysis/run_gaze_fusion.py`` and the :mod:`gaze` package;
see :ref:`analysis-api-gaze` for the module reference.

The method has four parts, each fixing a specific weakness of a single-camera
gaze heuristic:

1. **Metric head pose**, from MediaPipe's own metric face model and the
   cameras' real calibration, fitted jointly across every camera that sees
   the face.
2. **A geometric eyeball model**, which turns the iris seen in the image into
   an eye rotation, separately from the head's rotation.
3. **Robust fusion** of every camera's view of each eye, with an honest
   uncertainty.
4. **Subjects and targets**: who is who across cameras and time, and what
   each gaze ray lands on.

Conventions
-----------

All lengths are millimetres. Three frames are used:

.. list-table::
   :header-rows: 1
   :widths: 18 82

   * - Frame
     - Definition
   * - Head
     - +x towards the subject's left, +y down, +z into the head. The face
       looks along **-z**.
   * - Camera
     - OpenCV: +x right, +y down, +z forward.
   * - Room
     - The reference camera's frame, chosen during room calibration
       (:doc:`room_calibration`). ``extrinsic_rt`` maps camera to room:
       :math:`p_{room} = R\,p_{cam} + t`.

Gaze angles in the head frame are a yaw (positive towards head +x) and a
pitch (positive up); straight ahead is :math:`(0, 0)`.

Finding faces at room distance
------------------------------

MediaPipe FaceLandmarker carries its own face detector, a short-range model
built for faces filling a phone camera. Across a room a face is 70 to 90
pixels wide in a 1080p frame and that detector mostly finds nothing. So the
work is split: OpenCV's YuNet detector finds every face in the full frame,
then FaceLandmarker runs on a square crop 1.6 times the face's size (padded
where it leaves the frame), where the face is large again. Its normalised
landmarks map back to frame pixels as

.. math::

   u = x_0 + x_n\,s, \qquad v = y_0 + y_n\,s,

with :math:`(x_0, y_0)` the crop's corner and :math:`s` its side.

Head pose
---------

**Model.** MediaPipe ships a metric canonical face inside
``face_landmarker.task``: 468 vertices in centimetres, vertex *i* being
landmark *i*, plus a weighted list of 33 landmarks it treats as rigid (the
"Procrustes basis": nose, forehead, eye corners, cheekbones). The plugin
reads both directly from the bundle and converts them to the head frame
(:math:`\times 10`, y and z negated).

**One camera.** For the rigid landmarks with model points :math:`X_k` and
pixels :math:`x_k`,

.. math::

   \min_{R, t} \sum_k \left\| \pi\!\left(K, d;\ R X_k + t\right) - x_k \right\|^2,

where :math:`\pi` is the camera's real projection, including its distortion
:math:`d`. ``cv2.SOLVEPNP_SQPNP`` gives the global solution and
Levenberg-Marquardt refines it.

**Every camera together.** A face seen by several cameras gets a single
room-frame pose and a face scale :math:`s`:

.. math::

   \min_{R, t, s} \sum_{c} \sum_k w_k\, \rho\!\left(
     \left\| \pi_c\!\left( T_c^{-1} \left( R\,(s X_k) + t \right) \right) - x_{c,k} \right\|
   \right),

with :math:`T_c` camera *c*'s room-from-camera transform, :math:`w_k`
MediaPipe's rigid weights and :math:`\rho` a Huber loss (3 px), so a single
mislocated landmark or camera cannot pull the head away. The scale matters:
real faces are up to about 8% larger or smaller than the canonical one, and
one camera cannot tell a larger face from a nearer one. Two or more can.
The scale learned for a subject over all their multi-camera frames is
reused when only one camera sees them: scaling a face and its distance
together leaves the image unchanged, so the single-camera pose simply scales
in that camera's frame.

The eyeball model
-----------------

Each eye is a sphere rotating about a centre fixed in the head. From
average adult anatomy and the canonical face (whose eyelid surface sits about
3.6 mm in front of the eye-corner midpoint):

.. list-table::
   :header-rows: 1
   :widths: 45 15 40

   * - Quantity
     - Value
     - Source
   * - Rotation centre behind the eye-corner midpoint
     - 8 mm
     - centre about 13 mm behind the corneal apex
   * - Rotation centre to iris (pupil) centre
     - 9.5 mm
     - pupil plane about 3.5 mm behind the apex
   * - Visual axis from optical axis (kappa)
     - 5° nasal, 1.5° up
     - average adult; individuals vary by 2 to 3°

For eye centre :math:`c` (camera frame, from the head pose) and the
undistorted camera ray :math:`r` through the iris-centre pixel, the iris lies
where the ray meets the sphere of radius :math:`\rho = 9.5` mm:

.. math::

   t = r \cdot c - \sqrt{(r \cdot c)^2 - \|c\|^2 + \rho^2},
   \qquad p_{iris} = t\,r,
   \qquad a = \frac{p_{iris} - c}{\|p_{iris} - c\|}.

The near root is the front of the eye. If the ray misses the sphere (pixel
noise or a model error pushed it past the eye's silhouette), the sphere's
point nearest the ray is used, with less weight. :math:`a` is the optical
axis; expressed in the head frame it becomes the visual axis by adding kappa
to its yaw (towards the nose) and pitch.

Two safeguards keep small, noisy irises from producing absurd directions.
An eye-in-head rotation beyond 35° is pulled back to 35° and its weight cut:
eyes can turn about 45° but rarely go past 30°, because people turn the head
instead, so a larger estimate is almost always noise. And a closed eye (lid
gap under 12% of the eye's width) contributes nothing.

This separates head and eye rotation properly. Turning the head moves the
eye centre and the iris together and changes nothing in :math:`a` measured
in the head frame; only the iris moving relative to the centre does.

Fusing cameras and eyes
-----------------------

Every camera that sees an eye gives a visual axis :math:`v_{c,e}` (room
frame) with a reliability weight

.. math::

   w = w_{facing}\; w_{resolution}\; w_{open}\; w_{hit},

where :math:`w_{facing}` falls to zero as the eye turns more than about 80°
from the camera (each eye's own outward-turned normal is used, so the far
eye of a turned head goes first), :math:`w_{resolution}` grows with the iris
radius in pixels, :math:`w_{open}` with the lid gap, and :math:`w_{hit}` is
0.3 for a missed sphere or a limited rotation.

Each eye is averaged over cameras by a robust spherical mean: the weighted
mean, then every direction more than 20° from it dropped and the rest
re-averaged, up to three times. One camera that misread an eye cannot drag
the result.

The two eyes then count **equally**. They converge on what the subject
looks at, about 2.5° apart at 1.5 m, and weighting one more just because a
camera sees it better would tilt the result towards it. The gaze ray starts
at the midpoint of the two eye centres. When one eye is much less reliable
than the other (under a quarter of its weight: closed, or hidden behind the
nose), that eye alone is used and the ray starts **at that eye**; starting it
between the eyes would run it parallel to the true line of sight, 32 mm off.

**Uncertainty.** One pixel of iris noise moves the estimated rotation by
about :math:`\arctan(1/r_{px})`, with :math:`r_{px}` the iris sphere radius in
pixels. Combining the kept observations as independent measurements and
adding how much they actually disagree:

.. math::

   \sigma = \sqrt{\left(\sum_i \sigma_i^{-2}\right)^{-1} + \frac{D^2}{n}},

with :math:`D` the weighted RMS angle of the kept directions around their
eye's mean. This is the per-frame 1-sigma before smoothing, written as
``uncertainty_deg``.

Subjects across cameras and time
--------------------------------

**Association.** Each face first gets a head position from its own camera.
That position is uncertain mostly along the camera's line of sight (depth,
about 15%), much less sideways. So two faces from different cameras are the
same person when their sight lines (camera centre through the head) pass
within 150 mm of each other, **and** where they meet lies inside both
estimates' depth bands, **and** their midline landmarks triangulate with a
mean reprojection error under 25 px. Triangulation alone is not enough, a
fact found while testing: with cameras and seated heads at one height, every
pair of sight lines meets somewhere, so it matched neighbours; but for two
different people that meeting point lies far outside the depth bands. Pairs
join best first; a group grows only when every face in it fits every other
(a chain A~B, B~C never joins A to C on its own), with one face per camera.
If the joint head fit still explains the landmarks poorly (above 8 px RMS),
the faces are kept as separate subjects rather than merged into a compromise
head.

**Tracking.** Head positions are followed from tick to tick (a track may go
unseen for 2 s, and move at most 400 mm between sightings). Two tracks never
seen at the same tick are joined when every hand-over between them is
walkable: the head moved at most 0.5 m plus 1 m/s times the time between the
two sightings. That joins a seated person who looked away from every camera,
and someone who got up, walked around and came back; it never joins two
people seen at once. Two tracks that run side by side within 250 mm on most
shared ticks are one person seen twice, and are joined too. Tracks seen for
less than a second are dropped. Subjects are named ``S1``, ``S2``... from left to right in
the room, so names are stable between runs; ``gaze_targets.json`` can rename
them.

**Smoothing.** Origins and directions are smoothed per subject with a One
Euro filter (Casiez, Roussel and Vogel, 2012), whose cutoff rises with the
signal's speed: strong smoothing during fixations, little during saccades.
It runs forwards and backwards and the two passes are averaged, so it adds
no lag. Gaps longer than 0.5 s are never bridged.

What the gaze lands on
----------------------

Every candidate the ray meets is collected and the **nearest along the
ray** wins: looking at a person standing behind a screen lands on the screen.

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Target
     - Test
   * - Another subject
     - The angle between the gaze and the direction to their eye midpoint is
       below :math:`\max(\arcsin(r/d),\ 7°)`, with :math:`r = 110` mm and
       :math:`d` the distance. Faces are judged by angle, not by hitting a
       sphere: a head subtends only about 5° at 2.5 m, less than the gaze
       accuracy, so a surface test would miss most real looks.
   * - Named region
     - The ray meets the region's plane within its width and height.
   * - Plane
     - The ray meets the calibrated plane in front of the subject.
   * - None
     - A free point 1.5 m along the ray, labelled ``none``.

Labels are debounced: a new target must hold for 150 ms to replace the
current one, so a single noisy frame cannot flip "S1 looks at S2" to
something else and back. The type and label are debounced together. The
gaze point stays the measured one; the output's ``measured`` field shows
when it differs from the debounced label. **Mutual gaze** is when S1's
debounced target is S2 and S2's is S1 at the same tick.

Timeline
--------

When Frame Sync Repair has produced ``synced/`` videos, frame *k* of every
camera is tick *k*, and tick times come from the trigger log. Frames the
repair filled in for a camera that missed a tick repeat an older image; they
are never analysed (the last estimate is held) but are still drawn in the
annotated videos. Without ``synced/`` the raw videos are aligned through
``sync_manifest.json``, and a frame further than half a tick from its tick is
treated as missing rather than used out of time.

When a camera was recorded with a crop (interview mode) different from the
one its intrinsics were calibrated with, the principal point is shifted by
the difference. Calibrations made before the crop was recorded are assumed
to be full-frame, with a note in the log.

Accuracy and limits
-------------------

On synthetic scenes (three calibrated cameras, two subjects, the real
canonical face) the pipeline recovers the gaze exactly without noise; with
0.3 and 0.6 px of landmark noise the median error after smoothing is about
1.7° and 3.8°, and mutual gaze is detected on every frame.

Real footage is harder, and these limits matter:

* **Iris resolution.** At 2.5 m with a 1080p camera the iris is only 5 to 7
  pixels across; one pixel of landmark noise is about 10° of eye rotation.
  Several cameras and the smoothing average this down, but a single distant
  camera gives a coarse gaze. ``uncertainty_deg`` says how coarse.
* **Average anatomy.** Eyeball geometry and kappa are population averages; a
  given subject may be off by a few degrees consistently.
* **Not yet validated on calibrated footage.** On one uncalibrated room-11
  recording (one person looking down at a laptop, analysed with a
  placeholder focal length), the eye-in-head pitch read 11 to 20° upward.
  Whether that comes from the placeholder calibration, from MediaPipe's iris
  centre drifting up when a lowered lid covers the top of the iris, or from
  the eye-centre height in the model is not yet known. A short check with a
  calibrated room and known targets (look at a camera, at the partner, at a
  region, a few seconds each) is the way to settle it, and its result could
  feed a per-subject correction.
* **Identity** across cameras relies on head positions, so it needs the
  room calibration to be good. Without room calibration only one camera can
  be used (``--camera N``), in that camera's own frame.
