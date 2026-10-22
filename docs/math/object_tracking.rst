Object Tracking
===============

.. contents:: On this page
   :local:
   :depth: 1

Implemented in ``analysis/run_objects.py`` and the ``objects`` package
(``track.py``, ``contact.py``); shown in the Analysis tab's **Object
Tracking** entry and in the session report.

Finding objects by name
-----------------------

Objects are found and followed with **SAM 3** (Segment Anything Model 3,
Meta, 2025), run through ultralytics' ``SAM3VideoSemanticPredictor``. SAM 3
takes short noun phrases, "toy", "cup", "tablet" by default, finds every
instance of each in view, and follows each through the video as its own
**masklet** (a mask per frame with one id), adding instances that appear
later and ending those that leave. Each object keeps one id while it stays
in view, so "cup 1" and "cup 2" are told apart.

A phrase should name one thing ("toy", not "toys"), and say clearly which
thing: SAM 3 matches each instance against the phrase, and an ambiguous word
finds other things too. On a first room-11 recording "tablet" found the white
door panel behind the person, "tablet computer" did not; the defaults are
"toy, cup, tablet computer". A new object is started when SAM 3 scores it at
least 0.6 (its own default, 0.5, let more brief false objects through); one
already followed is kept while it scores at least 0.4. Objects seen for
less than 1 s in all are left out of the results and the annotated video
(the result says how many); the frames file keeps them, to check.
Every second frame is analysed by default (**every N frames**); following
objects and hands does not need every frame, and it halves the time. With
every N frames, the frames analysed are the Nth, the 2Nth, and so on, and
each stands for the N frames up to the next.

Each object is kept per frame as:

- its **box** and detection **score**;
- its **outline**: the largest region of its mask as a polygon, simplified
  (``cv2.approxPolyDP``, tolerance 0.5% of the perimeter, coarser until at
  most 64 points), so the result stays small and contact can be tested
  against the real shape, not only its box. An object whose mask comes back
  empty in a frame does not count as seen there.

SAM 3 also follows **hands**, under the phrase "hand", in the same run: one
more phrase, not a second pass over the video. Hands are not objects: they
are kept per frame next to the objects (``hands``), are not in the summary,
and are drawn thin and grey in the annotated video. Naming "hand" as an
object does nothing more.

Every frame's objects go to ``objects/video_N.objects.frames.json``, apart
from the summary in ``video_N.objects.json``, because they can be large.

Frame times come from the camera's ``timestamps_camN.csv``, the session's
elapsed clock, like every other analysis.

The weights, ``sam3.pt``, are gated: download them from Hugging Face
(``facebook/sam3``) after accepting Meta's SAM licence, and put them in the
MOSAIC folder. SAM 3's text encoder needs the ``clip`` package (ultralytics'
fork of OpenAI's CLIP), which the analysis environment includes.

Who touches what
----------------

A touch is found in two ways, and either is enough:

- **A hand on the object.** A hand's outline overlaps the object's, or comes
  closer to it than 5% of the hand's size (the square root of its outline's
  bounding box area), at least 3 px: the two masks meet at the fingers, and
  simplifying both outlines moves their edges by a few pixels. The test is
  done on the two outlines drawn as masks, so edges that cross between
  corners count too. This needs no Pose result, so it works on a close-up
  camera that sees only an arm, where Pose finds no person. On the first
  room-11 test (sub-31, Camera 5, a close-up of a table) SAM 3 found hands
  in 327 of 340 analysed frames, and Pose none.
- **A wrist on the object** (needs Pose on the same camera). The people of
  the nearest frame Pose analysed, within half of Pose's own step (Pose may
  analyse every Nth frame too), are matched against the objects: a wrist
  keypoint (COCO 9 or 10, seen with confidence above 0.5) lies inside the
  object's outline, or closer to it than a margin: the wrist sits a little
  back from the fingertips, which are what actually touch. The margin is a
  quarter of the object's size, and at least 12 px.

**Whose hand**: a hand belongs to the Pose person whose seen wrist is on or
nearest its outline, within the same wrist margin (of the hand's size).
When no wrist is, the touch is kept as "?" ("someone" in the app): Pose was
not run, or sees only an arm. When the same object is touched by a known
person and by "?" in one frame, it is the same touch seen both ways, and
"?" is dropped.

Frame by frame touches become **contacts**: runs of the same person and
object, joined across gaps of up to 0.3 s or one and a half analysed steps,
whichever is longer (a missed detection), each lasting to its last frame
plus one step, and kept when they last at least 0.2 s. The person is the
Pose plugin's subject id for that camera ("?" for a person Pose did not
track). When each object is seen is given the same way, as spans.

Per object, the result gives:

- **seen**: the share of analysed frames it was found in, first and last;
- its mean **area** and how far its centre **travelled**, pixels;
- its **contacts**, and seconds touched per person.

Limits
------

- **Look-alikes**: on that first recording an earring was found as a "toy"
  and a "cup" for 1.5 to 2 s, at confident scores. Real objects in a study
  are usually seen far longer; check the brief ones in the annotated video.
- **Names, not identities.** Two identical cups are two objects, but an
  object that leaves and comes back can get a new id.
- **Hidden objects**: an object covered by a hand or out of view is not
  seen; a toy held inside a closed hand may be lost.
- **2D contact**: a hand or wrist in front of an object, between it and
  the camera, also counts as a touch, and so does a hand resting right next
  to one. Several cameras would tell (a later step, with the room
  calibration).
- **Hands as objects**: a held object can be taken into the hand's mask, or
  a hand-sized object found as a hand; the annotated video shows both
  outlines, to check.
- **No person from the hand alone**: on a camera without Pose, touches by
  two people are both "someone".
- **Speed**: SAM 3 needs a GPU. On the room PC's RTX A4000 a 22 s video
  took 3 minutes at every second frame, about 8 times the recording's
  length, so a 10 minute recording takes about 80 minutes per camera.
- Not yet included: objects as gaze targets ("looks at the toy") and joint
  attention, which need the room calibration and 3D gaze
  (:doc:`gaze_fusion`, :doc:`social_interaction`).

References
----------

- Meta AI (2025). SAM 3: Segment Anything with Concepts.
- Ravi, N., et al. (2024). SAM 2: Segment Anything in Images and Videos.
  arXiv:2408.00714.
