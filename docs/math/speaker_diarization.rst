Speaker Diarization
=======================

.. contents:: On this page
   :local:
   :depth: 2

Implemented in :func:`diarize.pipeline.assign_speakers` — see
:doc:`/analysis_api` for the full API reference. The transcription
(faster-whisper) and diarization (pyannote.audio) stages that produce this
function's inputs are themselves pretrained models, not derived math —
this page covers only the pure combination step.

Max-overlap interval assignment
------------------------------------

Given a transcribed segment :math:`w = [w_s, w_e]` (from faster-whisper)
and a set of speaker turns :math:`\{t^{(j)} = [t^{(j)}_s, t^{(j)}_e]\}`
(from pyannote.audio), each carrying a speaker label, the overlap between
the segment and turn :math:`j` is the length of their intersection:

.. math::

   \text{overlap}_j = \max\!\big(0,\; \min(w_e, t^{(j)}_e) - \max(w_s, t^{(j)}_s)\big)

The segment is assigned whichever turn has the largest overlap:

.. math::

   \text{speaker}(w) =
   \begin{cases}
     \text{speaker of } \displaystyle\arg\max_j \text{overlap}_j
       & \text{if } \max_j \text{overlap}_j > 0 \\[4pt]
     \text{None} & \text{otherwise (no turn overlaps } w \text{ at all)}
   \end{cases}

This is the standard WhisperX-style recipe: simple interval-intersection
arithmetic, not linear algebra. A segment overlapping zero turns — or when
diarization was skipped entirely and the turn set is empty — gets
``speaker=None`` rather than a guessed label, consistent with this
project's general "don't overclaim" convention (see also
:doc:`facial_expression`'s tie-break).
