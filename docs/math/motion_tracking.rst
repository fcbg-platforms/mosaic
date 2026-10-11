Motion Tracking
==================

.. contents:: On this page
   :local:
   :depth: 2

Implemented in :mod:`motion.centroid_tracker` (detection + tracking) and
:mod:`motion.heatmap` (visualization, no additional math); see
:doc:`/analysis_api` for the full API reference. Run from the Session
Browser, not a live Analysis-tab plugin (see :doc:`/user_guide`).

Centroid extraction
-------------------------

After MOG2 background subtraction and morphological clean-up (see the
module docstring's full 7-step pipeline), each foreground contour's
centroid is computed from its image moments using the standard
area-weighted-average formula:

.. math::

   c_x = \frac{m_{10}}{m_{00}}, \qquad c_y = \frac{m_{01}}{m_{00}}

where :math:`m_{00}` is the contour's area (zeroth moment) and
:math:`m_{10}`/:math:`m_{01}` are its first moments along each axis.

Greedy nearest-neighbour assignment
-----------------------------------------

Each frame's detected blobs need matching to existing tracks. The cost
between every (track, detection) pair is plain Euclidean distance between
centroids:

.. math::

   C_{ij} = \lVert \text{pos}(\text{track}_i) - \text{pos}(\text{detection}_j) \rVert_2

Rather than solving this as a globally-optimal assignment problem (e.g.
the Hungarian algorithm), the tracker uses a **greedy** strategy: repeatedly
pick the global minimum-cost pair remaining in :math:`C`, accept it as a
match if that cost doesn't exceed ``max_distance``, then remove that row
and column from further consideration (by setting them to :math:`+\infty`)
and repeat. Tracks left unmatched after this loop get their
``lost_count`` incremented (and are pruned once it exceeds ``max_lost``);
detections left unmatched become new tracks (subject to the optional
``n_animals`` cap). A track that was not matched in a frame keeps its
identity while it waits, but writes no row for that frame: the output holds
only animals actually seen, never a repeated last position.

Each row's ``timestamp_ns`` is the frame's ``elapsed_ns`` from the camera's
``timestamps_camN.csv`` (the session's clock). Frames past the end of that
file continue from its last time at the video's frame rate; for a video
without one it is the frame's time in the video (frame index over frame
rate). With ``--skip N`` the speed is the distance moved over the N frames,
per second.

Each video is tracked on its own, and animal ids start again at 0 in every
video, so a row names its camera too: ``camera_index`` (from the file name,
``video_2.mp4`` is 2, shown as Camera 3), and an animal is the pair
``(camera_index, animal_id)``. In session mode the heat map, trajectory plot
and speed histogram are drawn per camera (``motion_heatmap_cam2.png`` and so
on), since positions from different cameras are in different views. Session mode reads only the
camera recordings, files named exactly ``video_N``; anything else in
``video/`` (a copy, an export) is skipped and named in the log.

.. note::

   Greedy assignment is simpler than an optimal solver and works well when
   animals are well-separated relative to their frame-to-frame movement,
   but, unlike a globally-optimal assignment, it can occasionally pick a
   locally-best pairing that isn't the best *overall* pairing when two
   tracks' plausible matches overlap. This is a deliberate simplicity
   trade-off, not an oversight.

Velocity: a simpler estimate than pose kinematics
--------------------------------------------------------

:meth:`~motion.centroid_tracker.Track.velocity_mm_per_s` converts the
pixel distance between a track's last two centroids into a real-world
speed using a **fixed nominal frame rate**, not each sample's real
elapsed time:

.. math::

   v = \frac{\lVert p_i - p_{i-1} \rVert_{\text{px}}}{n_i - n_{i-1}} \cdot (\text{mm/px}) \cdot \text{fps}

where :math:`n` is the frame each position came from: right after a gap
(the animal was lost for a few frames, then found again) the step is
divided over the frames it really took, rather than read as one frame's
movement. Contrast this with :doc:`pose_kinematics`, which uses each
sample's *real* timestamp delta. Motion tracking's frame-count estimate is a
deliberate, simpler choice for this plugin; it matches the real time when
the camera kept its frame rate, which is worth knowing if comparing speed
figures between the two plugins.

Practical recommendations
------------------------------

.. grid:: 1 1 2 2
   :gutter: 2

   .. grid-item-card:: 🎥  A static, uncluttered background helps most

      MOG2 background subtraction assumes the background is genuinely
      static: camera shake, lighting flicker, or moving background
      objects (a door, another person passing through) all produce
      spurious foreground blobs that can be mistaken for tracked subjects.
      A well-framed, stable shot does more for tracking quality than any
      parameter tuning.

   .. grid-item-card:: 🎚️  Tuning ``max_distance``

      Too tight and a fast-moving subject fragments into a new track every
      few frames; too loose and two nearby subjects can swap identities at
      a near-crossing. Set it based on how far a subject plausibly moves
      between frames at the recording's actual frame rate, not a generic
      default.

   .. grid-item-card:: 🔢  Set ``n_animals`` when the count is known

      Capping the expected subject count prevents spurious background
      noise from spawning phantom tracks; set it whenever the true
      number of tracked subjects in the session is known in advance.

   .. grid-item-card:: 📐  Prefer real Speed/Acceleration for a single tracked person

      If a session has exactly one subject and precise kinematics matter,
      :doc:`pose_kinematics`'s gap-tolerant, real-timestamp-based
      Speed/Acceleration is the more accurate choice. Motion tracking's
      fixed-fps velocity estimate is better suited to multi-subject
      centroid tracking than to precise single-subject kinematics.
