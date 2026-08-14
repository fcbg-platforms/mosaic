Mathematical Background
=========================

.. contents:: On this page
   :local:
   :depth: 1

This section derives the math behind each analysis capability, alongside
the code that implements it — for readers who want to know *why* an
algorithm produces the numbers it does, not just *how* to call it. For the
importable Python API itself, see :doc:`/analysis_api`; for how to run each
plugin from the app, see :doc:`/user_guide`.

Shared conventions
--------------------

Every page below that deals with 3D geometry (:doc:`gaze_fusion`,
:doc:`room_calibration`, :doc:`pose3d_reconstruction`) uses the same
conventions, matching
``room_frame::Mat4`` (C++) and ``ray_math.py``'s module docstring (Python):

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Convention
     - Meaning
   * - Units
     - Positions/lengths in **millimetres**; angles in the formulas below
       in **radians** (APIs that take degrees convert internally).
   * - Rigid transform
     - A 4×4 homogeneous matrix :math:`[R\,|\,t]`, applied as
       :math:`p_{\text{out}} = R \, p_{\text{in}} + t`.
   * - Storage
     - Row-major, flattened length-16 (index :math:`i \cdot 4 + j` is row
       *i*, column *j*; the bottom row is implicitly :math:`[0,0,0,1]`).
   * - Reference frame
     - Camera 0 is always the room-frame origin (identity transform);
       every other camera's extrinsic pose is relative to it.

.. grid:: 2 2 3 3
   :gutter: 3

   .. grid-item-card:: 🎯 Multi-Camera Gaze Fusion
      :link: gaze_fusion
      :link-type: doc

      3D ray construction, least-squares triangulation, ray-plane intersection.

   .. grid-item-card:: 📐 Room (Extrinsic) Calibration
      :link: room_calibration
      :link-type: doc

      Quaternion averaging and BFS pose-graph resolution across cameras.

   .. grid-item-card:: 🏃 Pose Kinematics
      :link: pose_kinematics
      :link-type: doc

      Gap-tolerant velocity/acceleration and why average speed is time-weighted.

   .. grid-item-card:: 🙂 Facial Expression
      :link: facial_expression
      :link-type: doc

      Weighted blendshape scoring and the FER+ softmax.

   .. grid-item-card:: 🗣️ Speaker Diarization
      :link: speaker_diarization
      :link-type: doc

      Max-overlap interval assignment between transcript and speaker turns.

   .. grid-item-card:: 🫥 Face Masking
      :link: face_masking
      :link-type: doc

      Box padding/clamping and blur-kernel sizing.

   .. grid-item-card:: 🐭 Motion Tracking
      :link: motion_tracking
      :link-type: doc

      Image-moment centroids and greedy nearest-neighbour assignment.

   .. grid-item-card:: 🧍‍♂️ 3D Pose Reconstruction
      :link: pose3d_reconstruction
      :link-type: doc

      Multi-view DLT triangulation, cross-camera person association, and 3D
      track identity.

   .. grid-item-card:: ❤️ Remote Heart Rate (rPPG)
      :link: remote_heart_rate
      :link-type: doc

      Green/CHROM/POS pulse extraction and Welch-periodogram BPM
      estimation. **Experimental.**

.. toctree::
   :hidden:

   gaze_fusion
   room_calibration
   pose_kinematics
   facial_expression
   speaker_diarization
   face_masking
   motion_tracking
   pose3d_reconstruction
   remote_heart_rate
