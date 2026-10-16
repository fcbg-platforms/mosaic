Voice Acoustics
===============

.. contents:: On this page
   :local:
   :depth: 1

The **Acoustics** pass (``analysis/run_voice.py``, the **▶ Acoustics**
button in the Analysis tab's audio view) turns each microphone's recording
into three things drawn under the waveform:

- a **spectrogram**: the sound's energy at each frequency, over time;
- the **pitch** of the voice (its fundamental frequency, F0);
- the **intensity** (loudness) contour.

It uses Praat, through the ``praat-parselmouth`` package. Every numeric
decision lives in ``analysis/voice/spectro.py`` (numpy only, unit-tested);
the calls into Praat are in ``analysis/voice/praat.py``. The pass needs no
transcript and no Hugging Face token, and it never re-runs Whisper.

.. figure:: /_static/voice_acoustics_example.png
   :alt: A spectrogram of three vowels, an s and a pause, with the pitch and
         intensity of the same sound below it.
   :width: 100%

   A synthetic utterance (no participant's voice), analysed with the same
   Praat calls and pitch clean-up as the app, and drawn at full resolution
   with pitch and intensity in their own panels (the app draws them over
   the spectrogram):
   the vowels [a], [i] and [u], an [s], and a pause. Each vowel shows
   bright horizontal bands, its **formants** (F1 to F3 marked for [a]); the
   [s] is noise high up, with no pitch; the pause is room tone, near the
   floor of the colour scale. Made by ``docs/figures/voice_acoustics_example.py``.

The spectrogram: a time-frequency picture
-----------------------------------------

A recording is a list of air-pressure samples; it says how the sound
changes in time, but not which frequencies it holds. A spectrum says which
frequencies a sound holds, but not when. A **spectrogram** has both: the
recording is cut into short, overlapping windows, the spectrum of each
window is computed (a short-time Fourier transform), and the spectra are
laid side by side. Time runs across, frequency up, and the energy at each
time and frequency is the brightness:

.. math::

   S(t, f) = \left| \int x(\tau)\, w(\tau - t)\, e^{-2\pi i f \tau}\, d\tau \right|^2

where :math:`x` is the sound and :math:`w` the window.

**The window sets what can be seen.** A window of length :math:`T` cannot
tell apart frequencies closer than about :math:`1/T`, and cannot place an
event in time more finely than :math:`T`. One has to choose:

- A **long window** (30 ms or more, "narrowband") resolves the individual
  harmonics of the voice, the multiples of the pitch, as thin lines; but
  each syllable's start and end blur.
- A **short window** (a few ms, "broadband") cannot separate the
  harmonics, which merge into broad bands. Those bands are the
  **formants**: resonances of the throat and mouth that make one vowel
  differ from another. It also shows each opening of the vocal folds as a
  thin vertical striation, and the exact start of every sound.

MOSAIC uses Praat's broadband default: a Gaussian window of **5 ms**
effective length (``WINDOW_LENGTH_S``), about 260 Hz of frequency
resolution. That is the standard picture for reading speech: vowels by
their formants, consonants by their noise and silences, voicing by its
striations.

**Analysis settings.**

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Setting
     - Value, and why
   * - Channels, rate
     - Mixed to mono, resampled down to 16 kHz if higher. Speech above
       8 kHz carries little a reader uses; mono first because Praat may
       otherwise silently analyse only the first channel.
   * - Frequency range
     - Computed to 7.84 kHz (8 kHz asked for, capped at 0.49 times the
       16 kHz rate so the top row is not reconstruction noise), shown 0 to
       5 kHz by default: Praat's own default view, where the formants of
       speech lie.
   * - Time step
     - 2 ms for recordings up to 10 minutes, 4 ms to 30 minutes, 8 ms beyond
       (the picture has a fixed number of columns, so a finer step on a long
       recording would only be averaged away).
   * - Frequency step
     - 20 Hz.
   * - Long recordings
     - Analysed in 30 s pieces, each extended by four windows at both ends
       and trimmed again, so no seam shows every 30 seconds.

**From power to a picture.** Praat returns power. It is converted to
decibels, :math:`10 \log_{10} S` (floored at :math:`10^{-14}`, about
:math:`-140` dB, so digital silence does not become :math:`-\infty`), and
reduced to an image of 256 rows and about 20 columns per second (at least
512, at most 4096, so an hour-long recording costs no more to draw than a
three-minute one):

- **Rows** (frequency): the power of the source rows falling in each image
  row is **averaged in power**, then converted to dB. Averaging decibels
  would average logarithms, which pulls every band towards its quietest
  bin and darkens the picture.
- **Columns** (time): of the analysis frames falling in each column, the
  **loudest** value is kept (in dB). When one column spans many frames, an
  average would smear each short sound (a "t", a click) into the silence
  around it; the maximum keeps it, as an audio editor shows it.

The image shows the **70 dB** below a ceiling set at the 99.9th percentile
of the recording's values (not the maximum, so one clipped sample or a
door slam does not darken everything else), stored as 8-bit grey (0.27 dB
per level) and drawn with the *magma* colour map: dark is quiet, bright is
loud.

Pitch
-----

The pitch, or fundamental frequency F0, is how often the vocal folds open
and close; it is heard as how high the voice is. It exists only while the
voice is **voiced** (vowels, m, n, l, ...); an [s], an [f] or a whisper has
none. Praat's autocorrelation method looks, in each 10 ms frame, for the
delay at which the sound best matches a delayed copy of itself; that delay
is one period, :math:`F_0 = 1/\text{delay}`.

Settings, and the changes from Praat's defaults:

- **Search range**: first 60 to 600 Hz. Then, with at least 50 voiced
  frames, a second pass narrows it to the voices present: from 0.75 times
  the 10th percentile (kept within 50 to 300 Hz) to 1.5 times the 90th
  (at least 50 Hz above the floor, at most 800 Hz). The app always does
  this; on the command line it takes ``--auto-pitch-range``. A
  fixed range either cuts off a low, creaky voice or a high one; a narrower
  one also halves or doubles the pitch less often.
- **Voicing threshold** 0.50 (Praat: 0.45), and the sound scaled to 70 dB
  first: ventilation hum and room tone are periodic enough to pass as voice
  at the default, and Praat's silence threshold is relative to the file's
  loudest moment, so one clipped sample would otherwise silence everything
  else.
- **Quiet frames dropped**: a pitch reading in a frame more than 35 dB
  quieter than the recording's loud level (its 95th percentile) is not
  someone speaking and is removed.
- **Very short voiced stretches dropped**: under 3 frames (30 ms), shorter
  than any real voicing; otherwise each leaves an isolated dot.

A gap in the pitch line means unvoiced or silent, never missing data, and
the line is broken there rather than drawn across.

The pitch line is drawn over the spectrogram on **its own scale**:
logarithmic, from the search range's floor at the bottom to its ceiling at
the top, so equal musical intervals look equal. It is not read against the
spectrogram's frequency axis (a voice at 120 Hz would be a hair above the
bottom of a 0 to 5 kHz axis).

Intensity
---------

The intensity is the sound's level in each frame, in dB, from Praat with a
32 ms window (for a lowest expected pitch of 100 Hz) every 10 ms. It is
relative, not a calibrated sound pressure level: the microphone's gain is
unknown. Compare it within a recording, not between recordings or rooms.

For long recordings the stored tracks are thinned (to at most 60 000 points
each): intensity by the mean of each block, pitch by the median of the
block's voiced values, or unvoiced when fewer than half of them are. A plain
mean would average real hertz against the unvoiced zeros and drag the
contour down.

Reading the picture
-------------------

- **Vowels**: bright horizontal bands, the formants, with fine vertical
  striations (each a vocal-fold pulse). The first two formants tell the
  vowels apart: [a] has F1 high and close to F2, [i] F1 low and F2 high,
  [u] both low.
- **Hissing sounds** ([s], [sh], [f]): noise, no striations and no pitch;
  [s] is highest (above about 4 kHz), [sh] lower.
- **Stops** ([p], [t], [k]): a short silence, then a thin vertical burst.
- **Pauses**: dark, at the room's noise floor. Ventilation shows as a
  steady low band throughout.
- **Pitch**: rises on questions and emphasis, falls at the end of
  statements; two voices in turn show as jumps between two ranges.
- **Overlapping speech**: two sets of striations and formants at once; the
  pitch tracker then follows whichever voice is stronger.

Files
-----

Per microphone, beside the audio: ``<mic>.voice.png`` (the spectrogram as
8-bit grey, top row the highest frequency) and ``<mic>.voice.json``: the
image's time and frequency span and its dB range, the pitch track (with
its search range and whether it was narrowed automatically) and the
intensity track, each with its start time and step. A later run reuses
them while both files exist (``run_voice.py --force`` recomputes).

Limits
------

.. grid:: 1 1 2 2
   :gutter: 2

   .. grid-item-card:: 🎙️  One microphone hears everyone

      The spectrogram and the tracks are of the whole recording, not of
      one person: both voices appear. Use the speaker colours of the
      diarized turns to tell whose speech is whose.

   .. grid-item-card:: 🎚️  Levels are relative

      Intensity and the spectrogram's brightness depend on the microphone,
      its gain and the distance to it. Compare within a recording.

   .. grid-item-card:: 🎵  Pitch errors

      The tracker can jump an octave (half or double the true pitch) on
      creaky or breathy voice, laughter or overlapping speech. Check the
      spectrogram's striations when a jump looks suspicious.

   .. grid-item-card:: 🔍  Detail on long recordings

      The picture has at most 4096 columns, so on a one-hour recording one
      column is about a second, showing the loudest moment within it. To
      see single sounds, analyse a short excerpt:
      ``run_voice.py --audio excerpt.wav --auto-pitch-range``.
