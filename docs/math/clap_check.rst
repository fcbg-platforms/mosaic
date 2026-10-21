Audio-Video Check
=================

.. contents:: On this page
   :local:
   :depth: 1

Implemented in ``analysis/run_clap_check.py`` and ``sync_check/clap.py``;
run from the Analysis tab's **Audio-Video Check** entry, and shown in the
session report's Data quality section.

The question
------------

MOSAIC's cameras are triggered together, so their videos agree with each
other. The audio is recorded apart and placed on the video clock afterwards:
from the recorder's timing file (``audio/<name>.timing.csv``, the sample
count and the time at each buffer's arrival), else from the session's start
(see :doc:`conversation_timing`). Every analysis that uses sound with images
relies on that placement: who speaks, response times, heart rate while
speaking. A **clap** checks it: one instant, both heard (a sharp sound) and
seen (two hands meeting). The audio time minus the video time of the same
clap is the offset still left in the placement.

Hearing a clap
--------------

The audio is brought to 16 kHz and cut into 25 ms windows every 4 ms. In
each, the log magnitude of the band from 1.5 to 7 kHz is taken (above most
of the voice's energy), and the **spectral flux**, the sum of the rises from
one window to the next, measures energy *starting* rather than loud sound,
so a clap across the room counts like one nearby. An onset is a flux peak at
least 6 times the flux's median over the second around it. Of two onsets
within 0.5 s the weaker is dropped (an echo never outranks its clap). This
follows the onset detection of the 4by4k pipeline (``fourk/synccheck.py``,
MIT licence).

The window's time is up to a window early (the flux peaks while the clap is
somewhere inside it), so each onset is refined on the samples: in the clap
band, the first sample whose level (a 0.5 ms running maximum of the
band-passed signal) rises half way from the background before to the peak.
On synthetic clicks at 16, 44.1 and 48 kHz this finds them within 1 ms.

The onset is placed on the video clock through the timing file's clock
line, or, for older recordings without one, from the session start: the
placement this check is about. (Conversation Timing additionally refines
an older recording's placement from the face on camera; this check does not,
so it shows the placement before that refinement.)

Seeing a clap
-------------

From a camera's Pose result: per frame, the distance between the main
person's wrists (COCO 9 and 10, both seen with confidence above 0.3) in body
heights (the person's box). A clap is a stretch of frames where the gap is
0.12 or less, timed at its lowest frame, when the gap was at least 0.1
wider within the previous half second and since the previous stretch (hands
brought together, not resting together). A wide swing is not needed:
clapping with the palms, the wrists stay about 0.1 apart and close to about
0.06. One stretch gives one clap, so a wobble while the hands stay together
is not a second one. Its time is refined between frames from the dip's
shape: the lowest point of a parabola through the lowest frame and its
neighbours.

Matching and the verdict
------------------------

First one offset for the whole recording: every heard-minus-seen
difference within 2 s votes, in 20 ms bins (a difference counts for the
neighbouring bins too), and the offset most seen claps agree on wins. It is
believed only when at least two seen claps, and at least half of them,
agree: with a poor view of the hands (a blurred camera, wrists half hidden)
Pose sees false claps, and a pair of those can line up with sounds by
chance. Such a camera then reports no matched clap rather than a wrong
offset. Pairing each clap with its nearest
sound instead would pair a clap with its neighbour when the audio is placed
about a clap's spacing off, and call that fine. Then each seen clap is
paired with the heard one nearest that offset, within 0.15 s, each heard
clap used once. Per camera and microphone the result gives the **median
offset** (audio minus video, ms), its **spread** (the median distance of the
offsets from it), and a verdict against that camera's frame interval (from
its own timestamps, not the Pose result's, which may skip frames):

- **ok**: within half a frame (20 ms at 25 fps): the audio lands on the
  right frame;
- **check**: within a frame;
- **off**: more than a frame: sound and image are paired wrongly, and
  analyses that use both (who speaks, response times) are off by that much.

A positive offset means the audio is placed late.

How to record for it
--------------------

Clap three times, clearly, with uneven gaps (clap, 1 s, clap, 2 s, clap),
hands in front of you and in view of the cameras, at the start of a
recording. Uneven gaps make the pattern impossible to match one clap
off. Run Pose on the cameras, then the Audio-Video Check.

Limits
------

- **Sound travels**: about 3 ms per metre, so a microphone 3 m from the
  hands hears the clap about 9 ms after it happens; this is in the offset.
- **The seen clap is the wrists, not the palms**: the wrists stop a few
  milliseconds after the palms meet, and the Pose model places them to a
  few pixels; a frame interval (40 ms at 25 fps) limits the time to a
  fraction of a frame even after the refinement. Several claps average this.
- **Other sharp sounds** (a door, a dropped object) are heard but not seen,
  so they are never matched; a clap out of view is not seen.
