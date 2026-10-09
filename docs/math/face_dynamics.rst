Face Dynamics
=============

.. contents:: On this page
   :local:
   :depth: 1

The Face Dynamics plugin (``analysis/run_face_dynamics.py``) turns one
camera's video of a face into the behaviours researchers count in interviews
and conversations: **blinks**, **smiles** (and which of them are Duchenne
smiles), **brow raises**, overall **expressivity**, and **head nods and
shakes**. The rules live in ``analysis/face_dynamics/metrics.py`` (numpy only,
unit-tested on synthetic signals); the per-frame measurements come from
``analysis/face_dynamics/extract.py``.

It is built for **interview mode**: one camera, a large face, about 50 fps. A
blink lasts 100 to 300 ms, which is 5 to 15 frames at 50 fps, enough to
measure its length. At a room recording's 12 to 25 fps a blink is 1 to 7
frames, and with a face 60 to 90 pixels wide the eyelid and head-pose noise
is large. The plugin still runs on room cameras but says so in its log.

Per-frame measurements
----------------------

**Finding the face.** MediaPipe FaceLandmarker's own detector expects a face
filling the frame. As in the 3D gaze plugin (:doc:`gaze_fusion`), YuNet finds
the face. FaceLandmarker then runs on a square crop around it, scaled to
256 px. The next frame's crop is cut around this frame's landmarks, and
YuNet runs again only when the face is lost. When several faces are visible,
the largest is used.

Each frame gives:

- the 478 face landmarks;
- the 52 **blendshapes**: scores from 0 to 1 for facial movements, close
  relatives of the Facial Action Coding System's action units (``mouthSmile``
  is AU12, ``cheekSquint`` AU6, ``browInnerUp`` AU1, ``browOuterUp`` AU2);
- the **head pose**, from a PnP fit of MediaPipe's metric canonical face to
  the rigid landmarks (the same fit as :doc:`gaze_fusion`).

**Head angles.** With :math:`R` the head-to-camera rotation (head frame: the
face looks along :math:`-z`, :math:`+x` towards the subject's left, :math:`+y`
down), the face direction is :math:`f = R\,(0,0,-1)^\top` and the side axis
:math:`s = R\,(1,0,0)^\top`. Then

.. math::

   \text{yaw} = \operatorname{atan2}(f_x, -f_z), \qquad
   \text{pitch} = \operatorname{atan2}\!\left(-f_y, \sqrt{f_x^2 + f_z^2}\right), \qquad
   \text{roll} = \operatorname{atan2}(s_y, s_x).

All three are 0 when the face looks straight at the camera. Yaw is positive
towards image right, pitch positive looking up, and roll positive for a
clockwise tilt in the image. They are relative to the **camera**, not the
room: a camera mounted high sees a person looking straight ahead as looking
up. Without an intrinsic calibration, a nominal focal length (the image
width) is used. That changes the angles only slightly.

Blinks
------

**Eye aspect ratio** (Soukupová and Čech, 2016). With six eyelid landmarks
:math:`p_1` (outer corner), :math:`p_2, p_3` (upper lid), :math:`p_4` (inner
corner) and :math:`p_5, p_6` (lower lid):

.. math::

   \text{EAR} = \frac{\lVert p_2 - p_6 \rVert + \lVert p_3 - p_5 \rVert}{2\,\lVert p_1 - p_4 \rVert}.

It is about 0.25 to 0.35 for an open eye and near 0 for a closed one, but the
open value differs between people, and within a person with expression (a
smile narrows the eyes) and head pitch.

**Openness** therefore divides EAR by the person's own **open-eye
baseline**: the 80th percentile of EAR over a centred 6 s window. Blinks
take only a few percent of the time, so that percentile is the open eye. A
slow change, such as eyes narrowed by an 8 s smile, moves the baseline with
it instead of reading as a long blink.

**Detection.** A closure is a span where the two eyes' mean openness drops
below 0.5. It ends only when openness climbs back above 0.7 (hysteresis), so
noise around one threshold cannot split one blink into two. Both eyes are
needed: one eye alone is a wink or a tracking error. A frame without a face
ends a span.

- **Blink**: 50 to 500 ms.
- **Long closure**: longer than 500 ms (resting the eyes, or drowsiness).
- Shorter than 50 ms: ignored.

The **duration** runs from the first frame below 0.5 openness to the end of
the last frame before openness is back above 0.7. For a smooth closure that
is about two thirds of the full lid movement, so it is shorter than durations
measured from the first lid movement. Every event's duration (blinks, smiles,
brow raises, nods) counts its last frame in full, so a single-frame event
lasts one frame.

**Rates.** Blinks per minute use only the time the face was seen. The mean
interval between blinks leaves out any interval during which the face was
lost (a look away would otherwise add its whole length). **PERCLOS** is the share of those frames with
openness at or below 0.2 (eyes at least 80% closed), the standard drowsiness
index (Wierwille et al., 1994).

Typical spontaneous blink rates are 10 to 20 per minute at rest, and higher
while speaking (Bentivoglio et al., 1997).

Expressions
-----------

**Smile** (AU12): the mean of ``mouthSmileLeft`` and ``mouthSmileRight``
rises above 0.5 and stays above 0.35 (hysteresis) for at least 300 ms.

**Duchenne smile**: a smile during which the mean ``cheekSquint`` (AU6, the
cheek raiser that narrows the eyes) reaches 0.3. Ekman, Davidson and Friesen
(1990) found that smiles with AU6 go with felt enjoyment more than smiles
without it. A Duchenne smile is reported as well as its smile, so "2 smiles,
1 Duchenne" means one of the two had the cheek raise.

**Brow raise** (AU1+2): the mean of ``browInnerUp`` and the two
``browOuterUp`` above 0.5 (released at 0.35) for at least 100 ms. Raises
shorter than 600 ms are marked as **brow flashes**, the quick greeting and
emphasis signal (Eibl-Eibesfeldt, 1972).

**Smiling time** is the summed smile duration over the time the face was
seen.

**Expressivity** measures how much the face moves, whatever the expression.
For each expressive blendshape :math:`b_k` (brows, cheeks, mouth, nose, jaw;
not the eyelids or gaze, which move with blinks and looking around), its
neutral level :math:`r_k` is its 10th percentile over the recording. Then

.. math::

   E_t = \frac{1}{K} \sum_{k=1}^{K} \max(0,\; b_{k,t} - r_k).

Subtracting each person's own rest removes the constant offsets some faces
have (a resting brow that reads slightly lowered), so :math:`E` is 0 at rest
for everyone.

Head nods and shakes
--------------------

A **nod** is a repeated pitch movement and a **shake** a repeated yaw
movement.

1. **Smooth** each angle with a centred 100 ms moving mean, to remove
   frame-to-frame landmark jitter.
2. **Detrend**: subtract a 2 s rolling median, so slow changes in where the
   head points (turning to face someone) do not count.
3. **Find swings** with a zig-zag filter. Movement starts once the angle has
   spanned 1.5°, however slowly; the first swing starts where it left its
   resting value. After that a turning point counts only once the angle has
   come back by 1.5°, so jitter cannot split one swing into many small ones.
4. A **gesture** is at least two consecutive swings that each meet three
   conditions:

   - at least 4°;
   - 0.15 to 1 s long;
   - over the same swing, the other axis's range (highest minus lowest) is
     at most 1/1.5 of it (otherwise it is a look around, or a diagonal
     movement, not a nod).

**Head speed** is the frame-to-frame rotation rate of the smoothed angles in
degrees per second, as a measure of overall head activity.

Outputs
-------

Per camera, in ``<session>/face_dynamics/``:

- ``video_N.face_dynamics.json``: schema ``mosaic-face-dynamics-v1``.
  Contains per-frame signals (openness, smile, brow, expressivity, head
  angles and speed), every event with its start, end, peak and duration, and
  the summary. Read by the Analysis tab.
- ``video_N.face_dynamics.csv``: the per-frame signals plus all 52
  blendshapes, for your own analysis.
- ``video_N.events.csv``: one row per event.
- ``video_N.face_dynamics.mp4``: the video with eye and lip outlines (eyes
  turn red during a closure, lips amber during a smile), the head direction
  arrow, running counts, the current events in large type, and a 4 s
  eye-openness trace.

Limits
------

- **One face per video**: the largest. In interview mode that is the
  interviewee.
- **Faces turned far from the camera** are lost, and that time is not
  counted.
- **Thresholds are population defaults**, not calibrated per person. The
  baseline normalisation removes most individual differences in eye shape,
  but smile and brow thresholds apply to MediaPipe's scores as they are.
- **Blendshapes are a model's estimate**, not FACS coding by a trained coder.
  Treat smiles and brow raises as candidates to check in the annotated video
  when the result matters.
- **No real interview recording has been checked yet**: the rules are tested
  on synthetic signals, and the pipeline runs on room-camera recordings, where
  faces are small.

References
----------

- Soukupová, T. and Čech, J. (2016). Real-time eye blink detection using
  facial landmarks. *21st Computer Vision Winter Workshop*.
- Wierwille, W. W. et al. (1994). Research on vehicle-based driver
  status/performance monitoring. *NHTSA report DOT HS 808 247*.
- Bentivoglio, A. R. et al. (1997). Analysis of blink rate patterns in normal
  subjects. *Movement Disorders* 12(6), 1028 to 1034.
- Ekman, P., Davidson, R. J. and Friesen, W. V. (1990). The Duchenne smile:
  emotional expression and brain physiology II. *Journal of Personality and
  Social Psychology* 58(2), 342 to 353.
- Eibl-Eibesfeldt, I. (1972). Similarities and differences between cultures
  in expressive movements. In R. A. Hinde (ed.), *Non-verbal Communication*.
