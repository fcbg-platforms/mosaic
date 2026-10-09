Conversation Timing
===================

.. contents:: On this page
   :local:
   :depth: 1

The Conversation Timing plugin (``analysis/run_conversation.py``) measures
the timing of a conversation:

- who speaks when;
- how quickly each person answers;
- how often they talk over each other, pause, or give short "mm-hm"
  backchannels.

It needs the session's audio and one camera's view of a face. The face on
the camera is the **subject**; every other voice on the microphone is the
**other** speaker. In interview mode that is the interviewee and the
interviewer. Rules live in ``analysis/conversation/`` (numpy only,
unit-tested on synthetic conversations).

The timing of speech always comes from the **audio**, which is precise to
about 10 ms. The camera and the diarization labels only decide **whose**
speech it is.

Placing audio on the video clock
--------------------------------

Video frames carry ``elapsed_ns`` timestamps. Audio is one continuous WAV.
Since this release, the recorder writes ``audio/<name>.timing.csv`` beside
each WAV: after each buffer of sound arrives, the number of samples written
so far, :math:`k_i`, and the clock time of arrival, :math:`t_i`.

A buffer always arrives a little after it was recorded (buffering,
scheduling), never before. So the plugin fits the line

.. math::

   t(k) = t_0 + k\,s

with a slope :math:`s` that a few odd rows cannot tilt: the median slope
between rows about 10 s apart, refined by a Theil-Sen fit through the
earliest arrivals of each 10 s window. It then lowers the line to the
earliest arrivals: :math:`t_0` is set so that only 2% of the points lie
below the line.

If those per-window floors differ by more than 20 ms, the clocks stepped
apart during the recording. That happens when the program stalled for longer
than the sound card's buffer and samples were lost. The floors then become a
piecewise correction instead of one constant; the step is reported
(``clock_step_ms``) and logged. The slope also measures the sound card's clock against the computer's.
They drift apart by tens of parts per million, about 0.1 s per half hour,
which the line corrects.

**Older recordings** have no timing file. Their audio started when the
session did (``session_start_elapsed_ns``), give or take a few hundred
milliseconds. The plugin then cross-correlates mouth movement with the audio
level over lags of ±0.5 s and shifts the audio by the best lag. It does this
only when there is at least 10 s of speech with the face in view, and the
correlation peak is clear: at least 0.15, and 0.1 above the median over all
lags. Mouth movement naturally leads its own sound by a few tens of
milliseconds, so this is good to about 0.1 s. The output records which
method was used.

Finding speech
--------------

Speech is found by **Silero VAD**, a small neural voice-activity detector
that ships with faster-whisper (installed for Speaker Diarization). It runs
on the audio resampled to 16 kHz:

- speech probability threshold 0.5;
- speech and silence of at least 100 ms;
- 30 ms of padding at each edge.

The result is sampled on a 10 ms grid. Unlike an energy threshold, it does
not take fans, machinery or music for speech. If faster-whisper is not
installed, an adaptive energy threshold on the 150 to 4000 Hz band is used
instead.

Who is speaking
---------------

**Mouth activity.** Speech moves the jaw and lips at the syllable rate (3 to
8 per second). The mouth opening :math:`m(t)` is MediaPipe's ``jawOpen`` plus
the mean of ``mouthLowerDownLeft`` and ``mouthLowerDownRight``. It is read
from Face Dynamics' per-frame CSV when that exists; otherwise it is measured
here. Activity is the standard deviation of :math:`m` over a centred 0.4 s
window. A held smile or an open mouth at rest has no activity.

**Threshold.** While nobody speaks, the subject cannot be speaking, so mouth
activity during silence is this person's non-speech movement. The threshold
is its 95th percentile. Silence within 0.5 s of speech is left out, because
the window reaches into the speech. A conversation with less than 2 s of such
silence falls back to Otsu's method instead: the activity during speech is
split into two groups (the subject's speech and the other's), provided the
groups' mean log activities differ by at least 1. Activity above the
threshold is "mouth moving". Above twice the threshold it is "strongly
moving". Both masks are then shrunk by half the window (0.2 s) at each end,
which puts their edges back where the mouth started and stopped.

**With diarization.** If Speaker Diarization ran (``audio/<name>.transcript.json``
has speaker labels), each label gets a score: the share of its speech, with
the face in view, during which the mouth moved. The label that scores at
least 0.35, and at least 0.15 more than any other, is the subject. At least
two labels with 1 s of speech each are needed: a diarization that put both
voices under one label cannot tell them apart, and the mouth is used
instead. With a matched label:

- the labels decide who speaks;
- overlapping segments give overlapping speech;
- the subject also speaks during another label's speech when the mouth moves
  strongly (talking over);
- unlabelled speech goes to the mouth;
- an unlabelled stretch under 0.25 s next to a label takes that label.

**Without diarization**, or when no label matches:

- speech with the mouth moving is the subject's;
- speech with the mouth still, face in view, is the other's;
- a switch shorter than 0.25 s inside continuous speech goes back to the
  speaker around it;
- speech overlap cannot be seen, since only one label is possible per
  moment.

Speech while the face is out of view and unlabelled is not counted, and is
reported as ``unattributed_speech_s``.

Turns and transitions
---------------------

Definitions follow Heldner and Edlund (2010) and Levinson and Torreira
(2015).

- A **spurt** is one speaker's speech, with their own silences shorter than
  200 ms filled in.
- A **backchannel** is a spurt shorter than 1 s, at least half of it spoken
  over the other speaker, or all of it inside the other's spurt ("mm-hm",
  "yeah"). It does not take the floor.
- A spurt of 1 s or more entirely inside the other's spurt is an **overlap
  without a floor change**: a failed interruption, or a long backchannel.
- Every other spurt is part of a **turn**. A speaker's consecutive spurts
  form one turn; the silences of 200 ms or more between them are
  **pauses**.
- A **transition** is a change of turn. Its **floor transfer offset** (FTO)
  is

  .. math::

     \text{FTO} = t_{\text{start}}(\text{new turn}) - t_{\text{end}}(\text{previous speaker's speech under way}).

  Positive is a **gap**; negative is an **overlapping transfer**. One that
  starts at least 0.5 s before the previous speaker stops counts as an
  **interruption**.

The FTO is the standard measure of response time in conversation. Across
languages its typical value is about +200 ms (Stivers et al., 2009). Longer
gaps go with dispreferred or difficult answers (Kendrick and Torreira, 2015).

**A short answer in silence is a turn**: "Yes." after a question is a turn,
not a backchannel, because it is not spoken over the other speaker.

Summary numbers
---------------

Per speaker:

- speech time and its share of the conversation (first to last speech);
- turns and their median duration;
- pauses, pauses per minute of the speaker's turn time, and median pause;
- backchannels;
- overlaps without a floor change;
- the **response offset**: FTO statistics of transitions *to* this speaker,
  that is, how quickly they take the floor;
- interruptions made;
- words and words per minute, when a transcript exists. Each transcript
  segment goes to the subject if its diarized label is the subject's
  (otherwise to the other speaker), or, without a matched label, to whoever
  spoke most of it.

For the conversation: the count and FTO statistics of all transitions, gaps,
overlapping transfers, interruptions, overlap count and total, and the share
of silence.

Outputs
-------

Per camera, in ``<session>/conversation/``:

- ``video_N.conversation.json`` (schema ``mosaic-conversation-v1``):

  - the timing method and offset, and how speakers were attributed (with
    each label's score);
  - the summary;
  - every turn (with its words), transition, spurt and overlap;
  - the audio level and mouth activity every 50 ms for the app's chart.

  All times are ``elapsed_ms``, the clock of ``video/timestamps_camN.csv``.

- ``video_N.turns.csv`` and ``video_N.transitions.csv``.
- ``video_N.conversation.mp4``: the video with who is speaking, the last
  response offset, the current turn's words and a 12 s scrolling timeline.
  It has no sound track (OpenCV writes video only).

Accuracy and limits
-------------------

Tested on synthetic conversations: two text-to-speech voices placed at known
times, and a mouth signal that moves with the subject's speech.

- **Gaps between speakers** come out within about 50 to 100 ms of the truth.
  They are taken from the speech detector's edges.
- **Overlaps** need diarization. Their edges come from where the diarized
  segments start and end, so they are typically 0.1 to 0.2 s short.
- **No real interview has been checked yet.** The rules and thresholds are
  population defaults.
- **One microphone**: the first ``audio/*.wav`` unless ``--audio`` names
  another.
- **One face per camera.** In a room recording, each camera's analysis is
  about the largest face that camera sees. Two people both seen by one camera
  are not told apart.
- **Lip movement without speech**, such as chewing or mouthing words, while
  the other speaks can be taken for overlapping speech when diarization says
  otherwise. It moves the mouth "strongly" only when vigorous.
- Without diarization, the other person's speech while the subject's mouth
  happens to move (laughing, nodding with an open mouth) is given to the
  subject.

References
----------

- Heldner, M. and Edlund, J. (2010). Pauses, gaps and overlaps in
  conversations. *Journal of Phonetics* 38(4), 555 to 568.
- Levinson, S. C. and Torreira, F. (2015). Timing in turn-taking and its
  implications for processing models of language. *Frontiers in Psychology*
  6, 731.
- Stivers, T. et al. (2009). Universals and cultural variation in turn-taking
  in conversation. *PNAS* 106(26), 10587 to 10592.
- Kendrick, K. H. and Torreira, F. (2015). The timing and construction of
  preference: a quantitative study. *Discourse Processes* 52(4), 255 to 289.
- Silero Team (2021). Silero VAD: pre-trained enterprise-grade voice activity
  detector. https://github.com/snakers4/silero-vad
