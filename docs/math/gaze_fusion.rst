Multi-Camera Gaze Fusion
===========================

.. contents:: On this page
   :local:
   :depth: 2

Implemented in :mod:`gaze.estimator` (per-camera head-pose solve) and
:mod:`gaze.ray_math` (pure geometry — see :doc:`/analysis_api` for the full
API reference). This page derives the three stages: turning a solved head
pose into a camera-local 3D ray, transforming that ray into room
coordinates, and fusing several cameras' rays into one triangulated point
(optionally intersected with a target plane).

Stage 1 — camera-local ray construction
------------------------------------------

:func:`~gaze.ray_math.camera_ray_from_pose` takes a head pose
:math:`(R, t)` — solved per-camera via ``cv2.solvePnP`` against a generic
6-point 3D face model and that camera's real calibrated intrinsics — plus
the existing 2D iris-offset heuristic :math:`(\Delta_x, \Delta_y) \in
[-1, 1]^2` (the same signal the live, single-camera gaze path already
computes), and produces a 3D ray origin and direction in the **camera's**
local frame.

**Origin.** The ray starts at the eye-centre model point
:math:`o_{\text{model}}` (the midpoint of the two eye-outer-corner model
points), mapped into camera space by the solved head pose:

.. math::
   :label: gaze-origin

   \text{origin} = R \, o_{\text{model}} + t

**Direction.** Rather than trusting a monocular depth estimate for the eye
itself (unobservable from iris landmarks alone), the direction composes
two rotations: the head's own solved orientation, and a small eye-in-socket
perturbation bounded by tunable constants
:math:`\psi_{\max}` (``max_eye_yaw_deg``, default 30°) and
:math:`\varphi_{\max}` (``max_eye_pitch_deg``, default 20°):

.. math::

   \psi = \Delta_x \, \psi_{\max}, \qquad
   \varphi = \Delta_y \, \varphi_{\max}

.. math::

   R_{\text{yaw}}(\psi) = \begin{bmatrix}
       \cos\psi & 0 & \sin\psi \\
       0 & 1 & 0 \\
       -\sin\psi & 0 & \cos\psi
   \end{bmatrix}
   \qquad
   R_{\text{pitch}}(\varphi) = \begin{bmatrix}
       1 & 0 & 0 \\
       0 & \cos\varphi & -\sin\varphi \\
       0 & \sin\varphi & \cos\varphi
   \end{bmatrix}

Applied to the face model's own forward axis :math:`f = (0, 0, 1)^\mathsf{T}`,
then rotated into camera space by the solved head pose:

.. math::
   :label: gaze-direction

   d_{\text{model}} = R_{\text{yaw}}(\psi) \, \big( R_{\text{pitch}}(\varphi) \, f \big),
   \qquad
   d = \frac{R \, d_{\text{model}}}{\lVert R \, d_{\text{model}} \rVert}

When :math:`\Delta_x = \Delta_y = 0`, :math:`d` reduces exactly to the
head's own forward direction :math:`R f` — the eye perturbation vanishes.

.. note::

   This is deliberately a heuristic, not a claim of true stereo eye depth.
   It separates two genuinely different signals: *which way the head is
   pointing* (metric, solved via ``solvePnP`` against real camera
   intrinsics) from *which way the eyes are rotated within it* (a bounded
   heuristic perturbation) — strictly more information than the live
   2D-only estimator it replaces, which conflates the two.

Stage 2 — transform to room coordinates
-------------------------------------------

:func:`~gaze.ray_math.transform_ray_to_room` applies that camera's
extrinsic pose :math:`[R_e \,|\, t_e]` (from :doc:`room_calibration`):

.. math::

   \text{origin}_{\text{room}} = R_e \, \text{origin} + t_e,
   \qquad
   d_{\text{room}} = \frac{R_e \, d}{\lVert R_e \, d \rVert}

(the direction is rotated only — no translation applies to a direction
vector).

Stage 3 — multi-ray least-squares triangulation
----------------------------------------------------

Given :math:`N \ge 2` contributing cameras, each with a room-space ray
:math:`(o_i, d_i)`, :func:`~gaze.ray_math.closest_point_of_rays` finds the
single 3D point :math:`x^\star` minimizing the summed squared perpendicular
distance to every ray.

For ray :math:`i`, the orthogonal projector onto the plane perpendicular to
:math:`d_i` is :math:`P_i = I - d_i d_i^\mathsf{T}` (symmetric, idempotent:
:math:`P_i^2 = P_i`). The squared perpendicular distance from a point
:math:`x` to ray :math:`i` is :math:`\lVert P_i (x - o_i) \rVert^2`, so the
objective is:

.. math::
   :label: gaze-objective

   F(x) = \sum_{i=1}^{N} (x - o_i)^\mathsf{T} P_i \, (x - o_i)

Setting the gradient to zero:

.. math::

   \nabla F(x) = 2 \sum_i P_i (x - o_i) = 0
   \;\;\Longrightarrow\;\;
   \underbrace{\Big(\sum_i P_i\Big)}_{A} x = \underbrace{\sum_i P_i o_i}_{b}
   \;\;\Longrightarrow\;\;
   A x = b

:math:`A` is a 3×3 matrix, symmetric positive semi-definite, and positive
**definite** (invertible) whenever the contributing ray directions aren't
all parallel — the common case with :math:`\ge 2` cameras viewing the same
face from different angles. The implementation solves :math:`Ax=b` directly
via ``np.linalg.solve``, falling back to the Moore–Penrose pseudo-inverse
:math:`x^\star = A^{+}b` (the minimum-norm least-squares solution) when
:math:`A` is near-singular — e.g. nearly-parallel rays, a genuinely
degenerate configuration — rather than raising.

**Fit quality.** The reported residual is the RMS perpendicular distance
from :math:`x^\star` back to every contributing ray:

.. math::

   \text{residual}_{\text{rms}} = \sqrt{ \frac{1}{N} \sum_i \lVert P_i (x^\star - o_i) \rVert^2 }

For a single ray (:math:`N=1`), there's nothing to triangulate — the
function returns that ray's own origin with a residual of exactly 0.

Target-plane intersection
------------------------------

:func:`~gaze.ray_math.ray_plane_intersection` finds where the fused ray
:math:`x(u) = x^\star + u\,\bar d` (using the mean of the contributing
directions as the fused direction :math:`\bar d`) crosses a plane defined
by a point :math:`p_0` and unit normal :math:`n` (the room's reference
plane, set once during :doc:`room_calibration`). Substituting into the
plane equation :math:`n \cdot (x(u) - p_0) = 0` and solving for :math:`u`:

.. math::

   u = \frac{n \cdot (p_0 - x^\star)}{n \cdot \bar d}

The intersection is reported as ``None`` (no target point) in two cases:
:math:`|n \cdot \bar d|` below a small epsilon (the ray runs parallel to
the plane — no well-defined intersection), or :math:`u < 0` (the
intersection lies *behind* the ray's origin, i.e. the gaze direction
points away from the surface).
