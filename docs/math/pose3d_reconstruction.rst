3D Pose Reconstruction
==========================

.. contents:: On this page
   :local:
   :depth: 2

Implemented in :mod:`pose3d.triangulation` (multi-view DLT + outlier
rejection), :mod:`pose3d.association` (cross-camera person matching), and
:mod:`pose3d.tracker` (cross-frame track identity); see :doc:`/analysis_api`
for the full API reference. This page derives all three stages: turning
several cameras' independently-detected 2D keypoints (from the
:doc:`Pose <../analysis_api>` plugin's own ``.pose.json`` output) into a
single 3D skeleton per person, per frame, with a stable identity across
frames.

Unlike :doc:`gaze_fusion`, which only ever triangulates one subject's gaze
ray, 3D Pose Reconstruction has to solve a harder problem first: with
multiple people in a room, *which* detection in camera A is the same
physical person as *which* detection in camera B, before any triangulation
can even be attempted. That is what Stage 2 below (association) exists for.

Stage 1: multi-view DLT triangulation
-------------------------------------------

Setup and camera model
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Every camera's extrinsic pose (solved during :doc:`room_calibration`) is
stored as ``extrinsic_rt``, the row-major 4×4 rigid transform
:math:`\text{room\_from\_camera}` (see :doc:`index`'s shared-conventions
table). :func:`~pose3d.triangulation.invert_rt` inverts it once per camera
into ``camera_from_room``:

.. math::

   [R \,|\, t]^{-1} = [R^\mathsf{T} \,|\, -R^\mathsf{T} t]

reimplemented in pure Python (Python cannot call the C++
``room_frame::invert()`` directly) but required to stay mathematically
identical to it. The code review that landed this feature specifically
verified this equivalence, since a sign error here would silently
mis-triangulate every point without any obvious symptom.

A pixel observation :math:`(u_{\text{px}}, v_{\text{px}})` is first
undistorted into ideal, :math:`K`-free normalized camera coordinates via
:func:`~pose3d.triangulation.normalize_point` (``cv2.undistortPoints`` with
no ``P=`` argument). Because the input is already :math:`K`-free, the
per-camera projection matrix used below also carries no :math:`K` term,
just the top 3 rows of ``camera_from_room``:

.. math::
   :label: pose3d-projmat

   P_i = \big(\text{camera\_from\_room}_i\big)_{[0:3,\,0:4]}

The linear DLT system
~~~~~~~~~~~~~~~~~~~~~~~~~~

For a 3D point :math:`X` in room space (homogeneous
:math:`X_h = (X, Y, Z, 1)^\mathsf{T}`) and view :math:`i` with normalized
observation :math:`(u_i, v_i)`, the projection relation
:math:`\lambda (u_i, v_i, 1)^\mathsf{T} = P_i X_h` holds for some unknown
scale :math:`\lambda`. Eliminating :math:`\lambda` by cross-multiplying the
first two rows against the third gives two linear equations per view, both
homogeneous in :math:`X_h`:

.. math::
   :label: pose3d-dlt-rows

   \big(u_i \, P_i[2,:] - P_i[0,:]\big) \, X_h = 0
   \qquad
   \big(v_i \, P_i[2,:] - P_i[1,:]\big) \, X_h = 0

:func:`~pose3d.triangulation.triangulate_point_dlt` stacks two such rows per
contributing view into one :math:`2N \times 4` matrix :math:`A` (needs
:math:`N \ge 2` views; a single view has no depth information at all).
Since :math:`A X_h = 0` and :math:`X_h \ne 0`, the exact solution (under
noiseless observations) is any vector in :math:`A`'s null space; under real
pixel noise, the least-squares solution is the right-singular-vector of
:math:`A` associated with its **smallest** singular value:

.. math::

   A = U \Sigma V^\mathsf{T}, \qquad X_h = V_{[:,\,-1]}

recovered via ``numpy.linalg.svd``. The 3D point is then the
dehomogenized :math:`X_h`, :math:`X = X_h[0{:}3] / X_h[3]`. The function
returns ``None`` if the SVD fails outright or the homogeneous coordinate is
too close to zero (a genuinely degenerate configuration, e.g. all
contributing rays nearly parallel) rather than dividing by a near-zero
value.

Reprojection-error rejection
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~pose3d.triangulation.reproject_error_px` projects the triangulated
point back through each contributing camera's **real, distorted**
intrinsics (``cv2.projectPoints``; unlike the DLT solve itself, this step
deliberately does use the full distortion model, giving an interpretable,
directly-comparable pixel-distance metric) and reports the Euclidean
distance to that camera's actual observed pixel.

:func:`~pose3d.triangulation.triangulate_with_rejection` is the function
``run_pose3d.py`` actually calls. It triangulates once from every
visibility-filtered view and computes each view's reprojection error.
If every view is within ``max_reprojection_error_px`` (default 15 px) of
the point, that is the result. Otherwise, with 3 or more views, every pair
of views proposes a point (two rays, triangulated), and the largest set of
views that agree with one of those points within the threshold (ties: the
smallest total error) is kept. The point is refitted from that set, and any
view that then no longer fits is dropped too. The obvious rule, dropping the
view with the largest error, is wrong: a gross outlier pulls the
least-squares point towards itself, so a good view can end up with the
largest error (the observation behind ``triangulate_robust`` in the 4by4k
pipeline, ``fourk/geometry.py``, MIT licence, which drops one view by
leave-one-out). With six cameras two views can be wrong at once, and then
every leave-one-out refit still holds the other one; searching pairs does
not have that problem, and with at most 15 pairs it stays cheap. It runs
only for keypoints whose first fit fails.

Two views that still disagree cannot say which of them is wrong, so the
keypoint is left absent (``None``) rather than guessed, the same
"skip, don't fabricate" discipline :doc:`pose_kinematics` applies to
detection gaps. Every kept view therefore reprojects within the threshold.
The views dropped are returned (``dropped_views``) and counted per camera
in ``skeleton3d.json``'s ``quality`` block (below): a camera dropped far more
often than the others is usually miscalibrated or moved.

Stage 2: cross-camera person association
------------------------------------------------

With multiple people in frame, each camera's per-frame YOLO detections
arrive in **enumeration order**, not identity order: camera A's "person 0"
has no inherent relationship to camera B's "person 0". Two detections must
be matched by evidence before their keypoints can be triangulated together.

Pairwise cost via 2-view triangulation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The most classical answer to this problem is epipolar-line distance
(project one camera's point through the fundamental matrix into the other
camera's image, measure the perpendicular distance to the observed point).
:func:`~pose3d.association.pairwise_cost` deliberately does **not** do
this. Instead, for every keypoint index visible in both detections
(:math:`\ge` ``min_visibility``, default 0.1), it 2-view-triangulates that
keypoint (Stage 1's own machinery, reused directly, not a separate
fundamental-matrix code path) and averages the two cameras' reprojection
errors:

.. math::

   \text{cost}(a, b) = \frac{1}{|S|} \sum_{k \in S}
       \frac{\text{err}_a(X_k) + \text{err}_b(X_k)}{2}

where :math:`S` is the set of shared, sufficiently-visible keypoint
indices (requires :math:`|S| \ge` ``min_shared_keypoints``, default 4, else
the cost is :math:`+\infty`). This is a strictly stronger signal than a
per-keypoint epipolar distance for a *calibrated* rig: it directly asks "if
these two detections were the same rigid articulated body, how physically
consistent is the resulting 3D reconstruction," rather than just "is point
B near the epipolar line of point A." It also costs nothing extra to
implement, since it reuses Stage 1's exact triangulation/reprojection
functions with no separate math to keep in sync.

Hungarian assignment, then a real threshold
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~pose3d.association.match_camera_pair` builds the full
:math:`|A| \times |B|` cost matrix between one camera pair's detections and
solves it with ``scipy.optimize.linear_sum_assignment`` (the Hungarian
algorithm, which minimizes *total* assigned cost in polynomial time). Hungarian
assignment alone is not enough: it will always produce *some* one-to-one
matching, even between detections that are genuinely different people, if
that happens to minimize the total. ``max_pair_cost_px`` (default 20 px) is
the actual "is this plausibly the same person" gate, applied *after* the
solver: any assigned pair whose cost still exceeds the threshold is
discarded rather than accepted.

Clustering across every camera pair
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~pose3d.association.cluster_people` runs
:func:`~pose3d.association.match_camera_pair` over **every** pair of
cameras (:math:`\binom{N}{2}` pairs for :math:`N` cameras), not just
physically-adjacent ones, since a room-scale rig can easily have two
non-adjacent cameras with better mutual visibility of a given person than
two adjacent ones. Every accepted pairwise match unions its two
``(camera_index, person_index)`` nodes in a union-find structure; the final
clusters are the resulting connected components that span :math:`\ge 2`
distinct cameras (a single-camera "cluster" can't be triangulated at all,
and is dropped).

.. important::

   **Known limitation, not fixed:** any single accepted pairwise match
   unions its two nodes regardless of how many *other* camera pairs agree.
   With only 2 total cameras, two genuinely different people can (rarely)
   still pass ``max_pair_cost_px`` if their rays happen to nearly cross by
   coincidence (any two independent 3D rays have *some* closest point;
   reprojection error alone can't rule out a coincidental near-intersection
   the way a disagreeing 3rd independent view could). Rooms with 3+
   calibrated cameras are far less exposed, since a false-positive
   2-camera pairing only survives into the union if no other accepted pair
   contradicts it, but this is not structurally impossible even then. See
   :ref:`pose3d-recommendations` below.

Stage 3: cross-frame track identity
-----------------------------------------

Each analyzed tick produces zero or more triangulated person-clusters with
no inherent link to the previous tick's clusters. :class:`~pose3d.tracker.PersonTracker3D`
assigns a stable ``track_id`` via **greedy nearest-3D-centroid matching**
between consecutive ticks, the same idiom :doc:`motion_tracking`'s 2D
``CentroidTracker`` uses, deliberately reimplemented (not shared) for 3D
room-millimetre centroids, per this project's established
per-plugin-owns-its-math precedent.

At each tick, the full (existing-track :math:`\times` new-centroid)
Euclidean-distance cost matrix is computed, and the **globally smallest**
remaining distance is matched first, repeated until either no pairs remain
or the smallest remaining distance exceeds ``max_jump_mm`` (default 400):

.. math::

   \text{cost}(t, c) = \lVert \text{track}_t.\text{last\_centroid} - c \rVert_2

A track with no match this tick **ages**: it survives up to
``max_gap_ticks`` (default 5) consecutive misses (a person briefly occluded
or triangulation-poor for a few ticks keeps its identity) before being
dropped. Any centroid left unmatched after the greedy pass (a new person,
or a track that aged out) receives a fresh, monotonically-increasing
``track_id``.

From tracks to people
~~~~~~~~~~~~~~~~~~~~~

A person seen by too few cameras for more than ``max_gap_ticks`` loses their
track and comes back with a new one. On sub-32_ses-04 (room 11, one person,
mostly seen by two cameras) that made one person six tracks, four under a
second. :func:`~pose3d.people.join_tracks` then joins tracks into people,
in the order they start: a track continues the person whose last appearance
ended before it began,

- at most ``MAX_GAP_S`` = 3 s before, and
- at most ``MAX_JUMP_MM`` + ``WALK_MM_S`` :math:`\times` gap = 500 mm +
  1.5 m/s :math:`\times` gap from where it begins (each track's centre is
  the mean of its valid keypoints at its first or last tick),

the nearest such person if several. Tracks that overlap in time are never
joined, so two people seen together stay two. Each person entry gets
``person_id`` (1, 2, ...) next to its ``track_id``, and the result lists
``people``: each one's ``track_ids`` and seconds in 3D. The quality checks
below judge the tracker itself, so they count tracks, before the joining;
the log adds "N person(s) from M track(s)". Smoothing stays per track: it
never runs across a gap. The app shows people ("Person 1"); a result
without ``person_id`` shows one person per track.

The Charts tab's one-person charts come from the joined person
(``compute_person_kinematics``): the hip midpoint's speed by central
difference over neighbouring ticks within 0.25 s (never across a gap), and
the head (highest seen face point) and hip heights above the floor found as
in :ref:`pose3d-drawing-the-room`.

Its joint angles are flexion, in degrees: the angle between the two
segments' directions, so a straight limb is 0 and every angle is 0 standing
upright with the arms hanging, the usual clinical convention. The line
under the chart gives each side's median.

- **Knee**: hip to knee against knee to ankle.
- **Elbow**: shoulder to elbow against elbow to wrist.
- **Hip**: the trunk, from the shoulders' midpoint down to the hips', against
  hip to knee; about 90 sitting upright, more leaning forward.
- **Arm raise**: the trunk, downwards, against shoulder to elbow; 90 with the
  arm out, 180 overhead.

They use the smoothed keypoints when the run smoothed them, since a
keypoint's few centimetres of jitter make several degrees on a forearm. An
angle is missing at a tick where any of its points is, and is never filled
in; the chart bridges gaps under a second and breaks the line at longer
ones. The keypoints are the 2D detector's joint centres made 3D, so the
angles are only as good as those: a few degrees for a limb seen well by two
or more cameras, much worse when the limb is hidden (legs under a table) or
seen by cameras off their calibration. The angles of a joint flexing out of
its usual plane (a hip turned out) are its 3D angle, not split into
flexion, abduction and rotation, which would need markers or a body model.

Its movement measures are rates per second, by central difference over the
neighbouring ticks within 0.25 s like the speed (never across a gap), but
from the smoothed keypoints (the speed uses the raw hips, as it always has):

- **Hand speed**: each wrist's.
- **Knee and elbow bending speed**: the flexion's rate of change, positive
  while bending, negative while straightening.
- **Whole-body motion**: the mean speed of the keypoints seen at the tick and
  both its neighbours, a 3D version of the quantity of motion used in
  interaction research. A mean, not a sum, so it does not grow when more
  keypoints are seen; but it changes when a fast keypoint goes out of view,
  and 5 of COCO's 17 keypoints are on the face, so a head turn alone shows
  in it.
- **Head speed** (the ears' midpoint) and **head turning**: the angle the
  ear-to-ear line turns through, in any direction, so it needs no vertical.

The line under a movement chart gives the usual value (median) and a high
one, the 95th percentile by nearest rank; for bending speed, of its size.
Not the maximum: a rate's few largest values are mostly the keypoints'
noise. Differencing magnifies that noise, and a speed or a turn is a size,
so the noise adds up rather than averages out: a rate has a floor above
zero. On a room-11 recording (sub-39), over 2 s of sitting still, the
median whole-body motion was 0.13 to 0.16 m/s and the median head turning
12 to 13 degrees per second, the ears being only 15 cm apart. Compare
rates within a session, or against such a still period, rather than read
them as absolute.

Its **posture** (``classify_posture``) is, at each tick, from the hips
alone, since the legs are often hidden (under a desk, on sub-39 the knees
were never seen while sitting):

- **Sitting**: the hips' midpoint under 0.8 m above the floor. On a chair
  the hips are about 0.55 to 0.75 m up, standing 0.85 m or more for most
  adults (the hip joint is at about 53% of a person's height).
- **Walking**: otherwise, when the hips' speed is 0.35 m/s or more, as the
  median over the ticks within 0.5 s either side: one tick's speed is noisy
  (0.1 to 0.35 m/s sitting still on sub-39), slow walking about 0.3 to
  0.7 m/s.
- **Standing**: otherwise.
- **Unknown**: in 3D without the hips, or without a speed nearby.

Then each tick takes the most common posture among the ticks within 0.5 s
either side that have one (a tie keeps its own): a mode filter, so the
timeline does not flicker and a posture held for under about half a second
does not show. A clean change shows where it happens; noisy ticks near it
(a speed spike, a hip read low) can move it by up to half a second.
Unknown ticks stay unknown, never filled in. The chart has one row per
posture; the line under it gives the seconds in each.

The limits: the 0.8 m line suits adults of ordinary height on ordinary
chairs (a short person standing, or a tall person on a bar stool, can be
misread), and squatting or kneeling read as sitting. It depends on the
floor found from the ankles (:ref:`pose3d-drawing-the-room`). Shifting
weight while standing is standing, and slow steps under 0.35 m/s are too.
On sub-39 (a 31 s recording: a walk, then sitting at a desk) it gave
walking with short stops for the first 15 s, sitting from 17 to 26 s, then
getting up.

There is no trunk lean. It needs the true vertical, and the calibration does
not give one: the up of :ref:`pose3d-drawing-the-room` is the people's mean
torso, so someone slouching all session would lean 0 against it, and the
cameras' mean image-up and the plane of the ankles are 7 to 9 degrees from
it on a room-11 recording (sub-39). Hip flexion and arm raise are measured
against the trunk itself, so they do not need the vertical.

Quality checks
---------------

``skeleton3d.json`` carries a ``quality`` block, written by
:mod:`pose3d.audit`, and the run's log prints its totals. Frame by frame,
geometry cannot tell whether track 1 at one tick is the same person as
track 1 at the next; physics can. The checks follow ``temporal_audit`` in
the 4by4k pipeline (``fourk/assoc.py``, MIT licence).

``quality.identity`` (:func:`~pose3d.identity_audit`), per track and in
total:

- **jumps**: the body moving faster than 3 m/s between two observations
  at most 0.5 s apart. Its movement is the mean movement of the shoulders
  and hips seen both times (any keypoint seen both times when none of
  those is), so hips going in and out of view behind a table are not a
  jump. A seated
  or walking person stays well under 2 m/s, so a jump is a wrong
  association or a track that picked up someone else.
- **swaps**: two tracks jumping at the same tick where exchanging their
  labels would remove both jumps (the exchanged distances under half the
  kept ones): the tracker swapped their names.
- **size_violations**: ticks where a bone is more than 25% away from that
  track's median length. A person's bones do not change length, so these
  are bad keypoints or someone else's.
- **bone_length_cv**: per track, the median over bones of the standard
  deviation divided by the median length; a few percent is normal keypoint
  noise.
- **short_tracks**: tracks lasting under 1 s, usually fragments of a
  longer one.

``quality.views`` (:func:`~pose3d.view_agreement`), per camera: the
keypoints it contributed to (``used``), those it saw but was dropped from
as disagreeing with the other cameras (``dropped``, and the percentage),
and those it saw that no two cameras agreed on, so none was triangulated
(``unresolved``: with two cameras, which one is wrong cannot be told). A
camera dropped far more often than the others, or with many unresolved
keypoints, has usually moved since the room calibration, or was
calibrated poorly.

``quality.timeline`` (:func:`~pose3d.qa_timeline`), per second: when
someone is missing from 3D, and why. Every tick records how many cameras
had a frame and saw a person, the person detections (all cameras), how many
of them belong to a match across enough cameras, the people matched, and
the people reconstructed. Each tick then gets the first outcome that
applies:

1. ``no_frames``: no camera had a frame (a sync gap);
2. ``nobody``: no camera saw anyone, so there is no one to place;
3. ``not_triangulated``: someone was matched, but no keypoint of theirs
   agreed across the cameras (triangulation, Stage 1);
4. everyone in 3D: every detection belongs to a match, and every match was
   reconstructed;
5. ``single_view_only``: people were seen, but only one camera saw anyone;
6. ``one_view_or_unmatched``: two or more cameras saw people, but some
   detection belongs to no match. That is a person only one camera saw, a
   person the cameras did not match (association, Stage 2), or an extra
   detection (a double detection, a passer-by in one camera); without a
   fixed number of people these cannot be told apart.

Each second takes the outcome of most of its ticks, so a single flickering
tick does not mark the second. Seconds with the same cause join into
segments; ``seconds_by_cause`` sums them, ``nobody_s`` counts the seconds
without anyone, and ``complete_pct`` is the share of the seconds with
someone in view that had everyone in 3D (unknown, not 100%, when nobody
was ever seen). The session report draws it as a strip, and the run's log
prints the totals. It follows the quality timeline of the 4by4k pipeline
(``fourk/qa.py``, MIT licence), per session rather than per person, since
MOSAIC does not fix the number of people.

.. _pose3d-drawing-the-room:

Drawing the room
----------------------

The Analysis tab's room view (``Skeleton3DRoomViewW``, Qt Quick 3D:
``Room3DScene`` works out the scene, ``qml/Room3D.qml`` draws it) draws the
result in the room frame, which is the reference camera's own: x right,
y down, z forward. That frame has no floor and no "up" of its own, so both
are read from the result (``room_view_math.hpp``):

- **Up** is the mean direction from the middle of the hips to the middle of
  the shoulders, over every person and tick that has all four (a torso is
  upright far more often than not, sitting or standing). It needs at least
  10 such samples whose unit vectors average to a length over 0.5; with
  fewer, or torsos pointing every way, each camera's image-up (minus its y
  axis) is averaged instead, since room cameras hang about level; without
  camera directions, it is -y.
- **The floor** is the 5th percentile of the ankles' heights along up,
  less 80 mm (an ankle's own height): a few low outliers do not sink it.
  Without ankles, it is the lowest keypoint; without any keypoint, 2 m
  below the lowest camera.
- **Front** looks along the reference camera's viewing direction laid flat
  on the floor; **Side** is at right angles to it; **Top** looks down.

Each camera in ``skeleton3d.json`` carries ``rotation_room`` (room-from-
camera, rows; its third column is the viewing direction) and ``fov_deg``
(horizontal and vertical field of view, from the focal lengths and the
calibrated image size), so the view can draw what each camera sees.

.. _pose3d-recommendations:

Practical recommendations
------------------------------

.. grid:: 1 1 2 2
   :gutter: 2

   .. grid-item-card:: 📷  Camera count and placement

      3+ calibrated cameras, viewing a person from meaningfully different
      angles, gives both better per-point triangulation accuracy (more
      views constrain :math:`A` in the DLT solve) and structurally rules
      out the 2-camera false-positive-association risk above. This is the
      single highest-leverage recommendation on this page.

   .. grid-item-card:: 📐  Calibrate room extrinsics carefully first

      Every stage here depends entirely on accurate ``extrinsic_rt`` values
      from :doc:`room_calibration`. A camera with a marginal reprojection
      RMS there will silently degrade every downstream triangulation and
      association decision. Re-solve room calibration (more/better ChArUco
      shots) before tuning any of this plugin's own thresholds if results
      look physically wrong.

   .. grid-item-card:: 🎚️  Tuning ``max_reprojection_error_px``

      15 px is a reasonable default for typical GigE camera resolutions and
      realistic calibration RMS. Lowering it rejects more (potentially
      good) views as outliers; raising it accepts more but risks a visibly
      "floating" or jittery reconstructed point from a genuinely bad
      detection.

   .. grid-item-card:: 🧑‍🤝‍🧑  Tuning ``max_pair_cost_px`` and multi-person sessions

      The default (20 px) trades off false-merges (two people fused into
      one track) against false-splits (one person fragmented into several
      short tracks). If a session is known single-subject, association
      risk is moot, but for genuinely multi-person sessions with only 2
      cameras, consider tightening this threshold, and treat any
      single-frame identity swap as a plausible outcome to sanity-check
      against the room view, not just trust blindly.

   .. grid-item-card:: 🏃  ``PersonTracker3D`` tuning

      ``max_jump_mm`` (400 mm default) should roughly match how far a
      person can plausibly move between two analyzed ticks at the
      session's frame rate. Too tight fragments a fast-moving subject
      into new tracks; too loose can jump-assign one track onto a
      different nearby person. ``max_gap_ticks`` (5 default) is the
      identity-persistence budget across brief occlusion/triangulation
      failure; raise it for sessions with frequent short occlusions.

   .. grid-item-card:: ✅  Sanity-checking a result

      Limb lengths should stay roughly constant across frames for a real,
      correctly-triangulated skeleton. A limb that visibly stretches or
      snaps between frames is the clearest sign of either a bad
      association (wrong person merged) or an under-constrained
      triangulation (too few surviving views), not a data artifact to
      ignore.
