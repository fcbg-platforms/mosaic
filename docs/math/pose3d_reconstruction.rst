3D Pose Reconstruction
==========================

.. contents:: On this page
   :local:
   :depth: 2

Implemented in :mod:`pose3d.triangulation` (multi-view DLT + outlier
rejection), :mod:`pose3d.association` (cross-camera person matching), and
:mod:`pose3d.tracker` (cross-frame track identity) — see :doc:`/analysis_api`
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

Stage 1 — multi-view DLT triangulation
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
identical to it — the code review that landed this feature specifically
verified this equivalence, since a sign error here would silently
mis-triangulate every point without any obvious symptom.

A pixel observation :math:`(u_{\text{px}}, v_{\text{px}})` is first
undistorted into ideal, :math:`K`-free normalized camera coordinates via
:func:`~pose3d.triangulation.normalize_point` (``cv2.undistortPoints`` with
no ``P=`` argument). Because the input is already :math:`K`-free, the
per-camera projection matrix used below also carries no :math:`K` term —
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
:math:`N \ge 2` views — a single view has no depth information at all).
Since :math:`A X_h = 0` and :math:`X_h \ne 0`, the exact solution (under
noiseless observations) is any vector in :math:`A`'s null space; under real
pixel noise, the least-squares solution is the right-singular-vector of
:math:`A` associated with its **smallest** singular value:

.. math::

   A = U \Sigma V^\mathsf{T}, \qquad X_h = V_{[:,\,-1]}

recovered via ``numpy.linalg.svd``. The 3D point is then the
dehomogenized :math:`X_h`, :math:`X = X_h[0{:}3] / X_h[3]` — the function
returns ``None`` if the SVD fails outright or the homogeneous coordinate is
too close to zero (a genuinely degenerate configuration, e.g. all
contributing rays nearly parallel) rather than dividing by a near-zero
value.

Reprojection-error rejection
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~pose3d.triangulation.reproject_error_px` projects the triangulated
point back through each contributing camera's **real, distorted**
intrinsics (``cv2.projectPoints`` — unlike the DLT solve itself, this step
deliberately does use the full distortion model, giving an interpretable,
directly-comparable pixel-distance metric) and reports the Euclidean
distance to that camera's actual observed pixel.

:func:`~pose3d.triangulation.triangulate_with_rejection` is the function
``run_pose3d.py`` actually calls: it triangulates once from every
visibility-filtered view, computes each view's reprojection error, and — if
any view exceeds ``max_reprojection_error_px`` (default 15 px) — drops the
offending view(s) and re-triangulates **once** from the remainder. This is
deliberately a single pass, not an iterative RANSAC-style loop: matching
this codebase's "skip, don't fabricate" discipline (the same one
:doc:`pose_kinematics` applies to detection gaps), a genuinely bad
observation is better excluded once than chased through repeated refits
that could overfit to noise. If fewer than 2 views survive rejection, the
function returns ``None`` — a keypoint with insufficient agreement across
cameras is left absent, never guessed.

Stage 2 — cross-camera person association
------------------------------------------------

With multiple people in frame, each camera's per-frame YOLO detections
arrive in **enumeration order**, not identity order — camera A's "person 0"
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
keypoint (Stage 1's own machinery, reused directly — not a separate
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
B near the epipolar line of point A" — and it costs nothing extra to
implement, since it reuses Stage 1's exact triangulation/reprojection
functions with no separate math to keep in sync.

Hungarian assignment, then a real threshold
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~pose3d.association.match_camera_pair` builds the full
:math:`|A| \times |B|` cost matrix between one camera pair's detections and
solves it with ``scipy.optimize.linear_sum_assignment`` (the Hungarian
algorithm — minimizes *total* assigned cost in polynomial time). Hungarian
assignment alone is not enough: it will always produce *some* one-to-one
matching, even between detections that are genuinely different people, if
that happens to minimize the total. ``max_pair_cost_px`` (default 20 px) is
the actual "is this plausibly the same person" gate, applied *after* the
solver — any assigned pair whose cost still exceeds the threshold is
discarded rather than accepted.

Clustering across every camera pair
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~pose3d.association.cluster_people` runs
:func:`~pose3d.association.match_camera_pair` over **every** pair of
cameras (:math:`\binom{N}{2}` pairs for :math:`N` cameras) — not just
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
   With only 2 total cameras, two genuinely different people can — rarely —
   still pass ``max_pair_cost_px`` if their rays happen to nearly cross by
   coincidence (any two independent 3D rays have *some* closest point;
   reprojection error alone can't rule out a coincidental near-intersection
   the way a disagreeing 3rd independent view could). Rooms with 3+
   calibrated cameras are far less exposed, since a false-positive
   2-camera pairing only survives into the union if no other accepted pair
   contradicts it — but this is not structurally impossible even then. See
   :ref:`pose3d-recommendations` below.

Stage 3 — cross-frame track identity
-----------------------------------------

Each analyzed tick produces zero or more triangulated person-clusters with
no inherent link to the previous tick's clusters. :class:`~pose3d.tracker.PersonTracker3D`
assigns a stable ``track_id`` via **greedy nearest-3D-centroid matching**
between consecutive ticks — the same idiom :doc:`motion_tracking`'s 2D
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
dropped. Any centroid left unmatched after the greedy pass — a new person,
or a track that aged out — receives a fresh, monotonically-increasing
``track_id``.

.. _pose3d-recommendations:

Practical recommendations
------------------------------

.. grid:: 1 1 2 2
   :gutter: 2

   .. grid-item-card:: 📷  Camera count and placement

      3+ calibrated cameras, viewing a person from meaningfully different
      angles, gives both better per-point triangulation accuracy (more
      views constrain :math:`A` in the DLT solve) and structurally rules
      out the 2-camera false-positive-association risk above — this is the
      single highest-leverage recommendation on this page.

   .. grid-item-card:: 📐  Calibrate room extrinsics carefully first

      Every stage here depends entirely on accurate ``extrinsic_rt`` values
      from :doc:`room_calibration`. A camera with a marginal reprojection
      RMS there will silently degrade every downstream triangulation and
      association decision — re-solve room calibration (more/better ChArUco
      shots) before tuning any of this plugin's own thresholds if results
      look physically wrong.

   .. grid-item-card:: 🎚️  Tuning ``max_reprojection_error_px``

      15 px is a reasonable default for typical GigE camera resolutions and
      realistic calibration RMS — lowering it rejects more (potentially
      good) views as outliers; raising it accepts more but risks a visibly
      "floating" or jittery reconstructed point from a genuinely bad
      detection.

   .. grid-item-card:: 🧑‍🤝‍🧑  Tuning ``max_pair_cost_px`` and multi-person sessions

      The default (20 px) trades off false-merges (two people fused into
      one track) against false-splits (one person fragmented into several
      short tracks). If a session is known single-subject, association
      risk is moot — but for genuinely multi-person sessions with only 2
      cameras, consider tightening this threshold, and treat any
      single-frame identity swap as a plausible outcome to sanity-check
      against the room view, not just trust blindly.

   .. grid-item-card:: 🏃  ``PersonTracker3D`` tuning

      ``max_jump_mm`` (400 mm default) should roughly match how far a
      person can plausibly move between two analyzed ticks at the
      session's frame rate — too tight fragments a fast-moving subject
      into new tracks; too loose can jump-assign one track onto a
      different nearby person. ``max_gap_ticks`` (5 default) is the
      identity-persistence budget across brief occlusion/triangulation
      failure — raise it for sessions with frequent short occlusions.

   .. grid-item-card:: ✅  Sanity-checking a result

      Limb lengths should stay roughly constant across frames for a real,
      correctly-triangulated skeleton — a limb that visibly stretches or
      snaps between frames is the clearest sign of either a bad
      association (wrong person merged) or an under-constrained
      triangulation (too few surviving views), not a data artifact to
      ignore.
