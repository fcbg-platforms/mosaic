Room (Extrinsic) Calibration
================================

.. contents:: On this page
   :local:
   :depth: 2

Implemented in C++, not Python: :cpp:func:`mosaic::room_frame::average`
(quaternion averaging) and :cpp:func:`mosaic::room_frame::bfs_resolve`
(pose-graph resolution), both in ``src/calibration/room_frame_solver.hpp``.
See :doc:`/user_guide`'s calibration workflow for how to actually run this
from the app, and :doc:`/calibration` for intrinsic (per-camera lens)
calibration, which this depends on.

The problem
---------------

Given a set of Basler cameras, each already intrinsically calibrated
(:doc:`/calibration`), the goal is to find every camera's **extrinsic**
pose (its position and orientation relative to one shared "room" origin),
so gaze rays, or any other per-camera 3D signal, from different cameras
can be combined in one coordinate system (see :doc:`gaze_fusion`).

This is solved by moving a board through the room, capturing "shots"
where two or more cameras see the board simultaneously. For each camera
that sees the board in a given shot, a perspective-n-point solve against
that camera's real intrinsics gives a board→camera pose. The remaining
problem is: given many pairwise board→camera poses scattered across many
shots, find one consistent camera→room pose per camera.

The board's pose in each camera
--------------------------------

**Board points.** A ChArUco board names each corner by the codes around
it. A plain chessboard does not: its corners come back in an order that
depends on how the board is turned (a square board is the same after a
quarter turn, any board after a half turn), and, with some detectors, in
mirror order. Two cameras could then number the same corner differently
and solve the board in different places. With one corner square marked,
``detect_marked_chessboard()`` (``src/calibration/marked_chessboard.hpp``)
fixes the order:

1. The inner corners are found (OpenCV's sector-based detector, falling
   back to the classic one) and put in front-view handedness: the first row
   must run to the right of the first column, :math:`e_1 \times e_2 > 0`
   for the image vectors along them, else every row is reversed.
2. A homography from grid units to the image places the four corner
   squares, which lie outside the inner corners. Each is sampled over the
   middle 60% of the square and compared with the square diagonally inside
   it, which always has the same colour: the difference in mean grey plus
   the difference in spread. The marked square is the one that differs; it
   must differ by at least 18 grey levels and twice as much as the next
   corner square, else the view is not used.
3. The corners are renumbered by a turn (never a mirror) so the marked
   corner comes first. The board points start at the board's outer corner
   at the marked square, :math:`x` along the first row, :math:`y` down the
   first column, :math:`z` into the board, as for a ChArUco board.

**Pose.** A flat target seen from a distance has two poses that fit almost
equally well, the true one and one tilted the other way. The solve uses
IPPE (``SOLVEPNP_IPPE``), which returns both ranked by their fit, takes the
better and refines it (Levenberg-Marquardt). A pose whose reprojected
corners miss the detected ones by more than 2 px RMS is not used (blur, a
misdetection, a wrong board size).

Rotation averaging
-----------------------

A single camera pair sharing several shots yields several *candidate*
relative poses (small variations from board-detection noise) that need
averaging into one. Averaging **translations** is a plain arithmetic
mean, :math:`\bar t = \frac{1}{K}\sum_k t_k`. Averaging **rotations** is
not as simple, because unit quaternions double-cover :math:`SO(3)`: the
map from the unit quaternion :math:`q` to the rotation it represents is
2-to-1: both :math:`q` and :math:`-q` represent the *exact same*
rotation. A naive arithmetic mean of quaternions sampled around one
physical orientation can therefore inadvertently sum quaternions from
opposite hemispheres of that double cover and partially cancel toward
zero, even though every sample represents nearly the same rotation.

**Rotation → quaternion.** Each sample's 3×3 rotation matrix :math:`R` is
first converted to a unit quaternion via the numerically-robust branch of
Shepperd's method (branching on whichever of the trace or the three
diagonal elements is largest, to avoid dividing by a near-zero term).
For the common case :math:`\text{tr}(R) = R_{00}+R_{11}+R_{22} > 0`:

.. math::

   s = 2\sqrt{\text{tr}(R) + 1}
   \qquad (s = 4 q_w)

.. math::

   q_w = \frac{s}{4}, \quad
   q_x = \frac{R_{21}-R_{12}}{s}, \quad
   q_y = \frac{R_{02}-R_{20}}{s}, \quad
   q_z = \frac{R_{10}-R_{01}}{s}

(the other three branches pivot on whichever diagonal element is largest,
following the same standard derivation).

**Sign alignment.** Before averaging, every sample :math:`q_k` is
sign-aligned to the first sample :math:`q_{\text{ref}} = q_1`: if
:math:`q_{\text{ref}} \cdot q_k < 0`, negate :math:`q_k`. Only after this
alignment are the (now consistently-signed) quaternions summed and
renormalized:

.. math::
   :label: quat-mean

   \bar q = \frac{\sum_k \operatorname{sign}(q_{\text{ref}} \cdot q_k) \, q_k}
                  {\left\lVert \sum_k \operatorname{sign}(q_{\text{ref}} \cdot q_k) \, q_k \right\rVert}

.. important::

   :eq:`quat-mean` is a **sign-aligned renormalized mean**, not the exact
   chordal/Karcher mean of :math:`SO(3)`. It's only a valid approximation
   of that true mean when the input rotations are already close together,
   which holds here specifically because the samples being averaged are
   repeated shots of the same *static* rig, not an arbitrary set of
   rotations. This is a deliberate, documented trade-off, not a claim of
   general correctness.

The averaged quaternion is converted back to a 3×3 rotation matrix by the
standard formula, and combined with the arithmetic-mean translation into
one averaged rigid transform.

Pose-graph resolution
--------------------------------

With one averaged relative pose available for every camera *pair* that
shared at least one shot, the remaining step is to compose these into one
consistent pose per camera, relative to a single reference camera (camera
0, which is always the identity transform; see the :doc:`index`'s
shared-conventions table).

Let :math:`T_{\text{cam} \to \text{room}}` denote the transform that
defines a camera's extrinsic pose (mapping a point in that camera's local
frame into room coordinates). For a BFS edge from an already-resolved
``parent`` to an unresolved ``child``, using shot :math:`k`'s
board→parent pose :math:`T_{\text{board}\to\text{parent}}^{(k)}` and
board→child pose :math:`T_{\text{board}\to\text{child}}^{(k)}`:

.. math::
   :label: bfs-composition

   T_{\text{child}\to\text{room}}^{(k)}
   = T_{\text{parent}\to\text{room}} \circ T_{\text{board}\to\text{parent}}^{(k)}
     \circ \big(T_{\text{board}\to\text{child}}^{(k)}\big)^{-1}

Reading the right-hand side right-to-left makes the composition concrete:
a point in the *child* camera's local frame is mapped to the board's frame
by :math:`(T_{\text{board}\to\text{child}}^{(k)})^{-1}` (i.e.
:math:`T_{\text{child}\to\text{board}}^{(k)}`), then from the board's frame
to the parent camera's frame by :math:`T_{\text{board}\to\text{parent}}^{(k)}`,
then from the parent camera's frame to the room frame by
:math:`T_{\text{parent}\to\text{room}}`, which is exactly
:math:`T_{\text{child}\to\text{room}}`.

Every shared shot between the same camera pair produces one such candidate
via :eq:`bfs-composition`. They are combined with the quaternion mean
:eq:`quat-mean` above, robustly (``robust_average()``): after a first mean,
any candidate whose rotation differs from it by more than 2 degrees or three
times the median difference (whichever is larger), or whose position
differs by more than 20 mm or three times the median distance, is left out
and the mean taken again (always keeping at least half). One shot solved
as the mirror pose would otherwise pull the camera off.

Starting from the reference camera (identity), the resolved set grows one
camera at a time: each step resolves the unresolved camera whose link to an
already-resolved camera has the **most** shared shots, through that link. A
plain breadth-first traversal would take whichever link it met first, and
place a camera from a single shot with the reference camera when a dozen
shots with a neighbour were there. A camera
with **no** shot-chain back to the reference camera (e.g. it never shared
a simultaneous board view with anything already resolved) is reported
**unresolved** rather than silently assigned a meaningless identity pose,
so the calibration UI can flag it and ask for more overlapping shots.

Agreement between the cameras
------------------------------

The fit of each pose to its own view says nothing about whether the
cameras agree. After the solve, for every shot two or more resolved cameras
saw, each camera's view of the board is carried into the room,
:math:`T_{\text{cam}\to\text{room}} \circ T_{\text{board}\to\text{cam}}`,
and the board's four outer corners and centre compared with the mean of
the other cameras' (leave one out). The RMS distance over all such shots
is that camera's **agreement**, in millimetres: how far apart the cameras
put the same object, at the distances and angles the board was shown at. It
is a lower bound for what to expect: the board was seen clearly, from
fewer than all cameras at a time, and only where it was shown.

The room's reference plane
-------------------------------

Once every camera's extrinsic pose is known, the room's reference plane
(used by :doc:`gaze_fusion`'s target-point intersection) is defined for
free: lay the ChArUco board flat on the target surface, capture one more
shot, and reuse its already-solved board pose. For any resolved camera
:math:`c` that saw that shot directly, the board's pose in room
coordinates is :math:`T_{\text{board}\to\text{room}} =
T_{c\to\text{room}} \circ T_{\text{board}\to c}`. The plane point is that
transform's translation column; the plane normal is its rotation part's
3rd column (the board's own printed-face normal, since ChArUco object
points lie in the board's local :math:`Z=0` plane by construction).

Practical recommendations
------------------------------

.. grid:: 1 1 2 2
   :gutter: 2

   .. grid-item-card:: 🎯  Aim for a well-connected shot graph

      Every camera needs *some* path of shared shots back to camera 0,
      not necessarily direct. Move the board through overlapping pairs of
      camera fields of view deliberately, not just wherever's convenient;
      a camera left unresolved contributes nothing to any downstream 3D
      plugin (:doc:`gaze_fusion`, :doc:`pose3d_reconstruction`).

   .. grid-item-card:: 🔢  More shots per pair, not just more pairs

      Multiple shared shots between the same camera pair feed
      :eq:`quat-mean`'s averaging step directly; a pair with only one
      shared shot has no noise-averaging benefit at all. A handful of
      varied-angle shots per overlapping pair meaningfully improves that
      pair's resolved relative pose over a single shot.

   .. grid-item-card:: 📏  Check the reprojection RMS, not just "resolved"

      A camera reporting "resolved" with a poor reprojection RMS still
      silently degrades every downstream 3D computation through it. Treat
      a high RMS the same as an unresolved camera and recapture that
      camera's shots with more care (steadier board, less motion blur,
      better lighting) rather than accepting a technically-non-null but
      low-quality pose.

   .. grid-item-card:: 🧊  Why this rig can't move afterward

      The averaged extrinsics describe the cameras' positions *at
      calibration time*. Any physical camera move, even a small bump,
      invalidates every pose computed from it, silently, with no runtime
      check catching the mismatch. Recalibrate after any physical rig
      change, not just when results start looking wrong.
