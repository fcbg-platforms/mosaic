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

Practical recommendations
------------------------------

.. grid:: 1 1 2 2
   :gutter: 2

   .. grid-item-card:: 🔑  The Hugging Face token is required for speaker labels

      Without a valid token (with both gated pyannote models' terms
      accepted), transcription still runs but every segment's speaker is
      ``None`` — this is a graceful degradation, not a failure, so it's
      easy to miss that diarization silently didn't run. Check the
      transcript table's Speaker column isn't empty if speaker labels
      actually matter for the session.

   .. grid-item-card:: 🎙️  Model size vs. accuracy vs. speed

      **small** (default) is a reasonable balance. **tiny**/**base** are
      noticeably faster but meaningfully less accurate on accented or
      noisy speech; **large-v3** is the most accurate but slowest —
      reasonable for a short, important recording where transcription
      quality matters more than turnaround time.

   .. grid-item-card:: 👥  Min/max speaker-count hints

      If the number of speakers in a session is known in advance, setting
      both min and max speakers to that count gives pyannote's diarization
      a real constraint to work with, generally improving speaker-turn
      accuracy over leaving it unconstrained — worth setting whenever the
      session's speaker count is actually known.

   .. grid-item-card:: 🗣️  A boundary segment can flip between overlapping turns

      A whisper segment that straddles two speaker turns is assigned
      entirely to whichever turn it overlaps more — for a segment near a
      genuine turn-taking boundary, small VAD/diarization timing
      differences can flip which speaker "wins." Treat a speaker label
      right at a turn boundary as somewhat less certain than one deep
      inside a long single-speaker stretch.
