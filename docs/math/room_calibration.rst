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
pose — its position and orientation relative to one shared "room" origin —
so gaze rays, or any other per-camera 3D signal, from different cameras
can be combined in one coordinate system (see :doc:`gaze_fusion`).

This is solved by moving a ChArUco board through the room, capturing
"shots" where two or more cameras see the board simultaneously. For each
camera that sees the board in a given shot, ``cv2.solvePnP`` against that
camera's real intrinsics gives a board→camera pose. The remaining problem
is: given many pairwise board→camera poses scattered across many shots,
find one consistent camera→room pose per camera.

Rotation averaging
-----------------------

A single camera pair sharing several shots yields several *candidate*
relative poses (small variations from board-detection noise) that need
averaging into one. Averaging **translations** is a plain arithmetic
mean, :math:`\bar t = \frac{1}{K}\sum_k t_k`. Averaging **rotations** is
not as simple, because unit quaternions double-cover :math:`SO(3)`: the
map from the unit quaternion :math:`q` to the rotation it represents is
2-to-1 — both :math:`q` and :math:`-q` represent the *exact same*
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
   of that true mean when the input rotations are already close together
   — which holds here specifically because the samples being averaged are
   repeated shots of the same *static* rig, not an arbitrary set of
   rotations. This is a deliberate, documented trade-off, not a claim of
   general correctness.

The averaged quaternion is converted back to a 3×3 rotation matrix by the
standard formula, and combined with the arithmetic-mean translation into
one averaged rigid transform.

Pose-graph resolution (BFS)
--------------------------------

With one averaged relative pose available for every camera *pair* that
shared at least one shot, the remaining step is to compose these into one
consistent pose per camera, relative to a single reference camera (camera
0, which is always the identity transform — see the :doc:`index`'s
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
:math:`T_{\text{parent}\to\text{room}}` — which is exactly
:math:`T_{\text{child}\to\text{room}}`.

Every shared shot between the same camera pair produces one such candidate
via :eq:`bfs-composition`; these are combined with the quaternion-mean
:eq:`quat-mean` above into a single edge pose. Starting from the reference
camera (identity), a breadth-first traversal of the shared-shot graph
propagates a resolved pose outward to every reachable camera. A camera
with **no** shot-chain back to the reference camera — e.g. it never shared
a simultaneous board view with anything already resolved — is reported
**unresolved** rather than silently assigned a meaningless identity pose,
so the calibration UI can flag it and ask for more overlapping shots.

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
