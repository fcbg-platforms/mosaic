Remote Heart Rate (rPPG)
============================

.. contents:: On this page
   :local:
   :depth: 2

.. important::

   **EXPERIMENTAL.** This plugin is a research-grade heart-rate estimate
   only. It is **not a medical device** and has **not been clinically
   validated**. Camera-based pulse-rate estimation (remote
   photoplethysmography, "rPPG") is a real, published technique with known
   accuracy bounds under good conditions, but it is easily degraded by
   motion, lighting, and compression artifacts. See
   :ref:`rppg-recommendations` below before trusting any output number.

Implemented in :mod:`rppg.roi` (face-skin ROI extraction),
:mod:`rppg.algorithms` (pulse-signal combination), and
:mod:`rppg.hr_estimation` (frequency-domain heart-rate extraction); see
:doc:`/analysis_api` for the full API reference. This page derives the
physiological signal model and all three classical (non-deep-learning)
combination algorithms the plugin offers, plus the windowed Welch-periodogram
estimation that turns a pulse signal into a BPM number.

The physiological signal
-----------------------------

Remote photoplethysmography exploits a real, physical fact: as the heart
beats, blood volume in facial skin capillaries oscillates at the pulse
rate, producing a tiny, periodic change in how much light the skin
reflects, strongest in the green channel, since haemoglobin absorbs green
light more than red. The whole plugin's job is to recover this weak
periodic signal from an ROI's mean RGB values, which are otherwise
dominated by illumination, motion, and camera-noise variation orders of
magnitude larger than the pulse signal itself.

The physiological pulse band searched throughout is fixed at
:math:`[0.7, 3.0]` Hz, i.e. 42 to 180 BPM, covering adult resting through
moderate-exertion heart rate (``LOW_HZ``/``HIGH_HZ`` in ``run_rppg.py``).

ROI extraction
-------------------

:class:`~rppg.roi.MediaPipeFaceRoiExtractor` uses MediaPipe FaceLandmarker
to locate a fixed 9-point **lower-face** polygon (both cheeks plus the
region between them, deliberately avoiding eyes, eyebrows, and the mouth,
which move independently of the pulse signal via blinking/speech) and
computes the mean RGB value inside it for every processed frame:

.. math::

   \bar{c} = \frac{1}{|\Omega|} \sum_{(x,y) \in \Omega} I_c(x, y),
   \qquad c \in \{R, G, B\}

where :math:`\Omega` is the ROI polygon's pixel mask. The landmark indices
are a real, cited reference (SamProell/yarppg's ``FaceMeshDetector``,
itself attributed to Li, Chen, Zhao & Pietikäinen, CVPR 2014), verified
against a real implementation before being used, not invented from memory,
matching this project's established discipline for algorithmic claims. A
frame with no detected face contributes **no** sample at all. This is a
real, honest gap, never interpolated or fabricated.

Stage 1: pulse-signal combination
----------------------------------------

Given an analysis window's stacked per-frame ROI means (an
:math:`N \times 3` array of R, G, B columns), the plugin offers three
backends, all implemented in :mod:`rppg.algorithms`. All three depend on
:func:`~rppg.algorithms.normalize_channels`, which divides each channel by
its own temporal mean over the window:

.. math::
   :label: rppg-normalize

   C_n = \frac{C}{\overline{C}}, \qquad C \in \{R, G, B\}

removing each channel's own DC brightness level so the much smaller
pulse-induced color variation isn't swamped by illumination differences
between channels.

Green: naive baseline
~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~rppg.algorithms.green_signal` (Verkruysse, Svaasand & Nelson, 2008)
is simply the mean-centered raw green channel:

.. math::

   s = G - \bar{G}

No motion or illumination compensation at all. This is the original,
simplest rPPG method, kept as a fast baseline for comparison/debugging
rather than the default, since any camera or subject motion during the
window directly corrupts it.

CHROM: chrominance-based
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~rppg.algorithms.chrom_signal` (de Haan & Jeanne, IEEE TBME 2013)
builds two **chrominance** signals from the temporally-normalized channels
:math:`(R_n, G_n, B_n)`:

.. math::
   :label: rppg-chrom

   X_c = 3 R_n - 2 G_n, \qquad Y_c = 1.5 R_n + G_n - 1.5 B_n

derived from a skin-reflection model in which specular/illumination changes
affect :math:`X_c` and :math:`Y_c` proportionally, while the pulse
component does not, so a single alpha-tuned combination cancels most of
the shared illumination component while preserving the pulse signal:

.. math::

   \alpha = \frac{\operatorname{std}(X_c)}{\operatorname{std}(Y_c)},
   \qquad
   s = X_c - \alpha \, Y_c

.. note::

   **A real ambiguity, surfaced rather than silently resolved.** A
   reference implementation consulted while verifying this formula
   (``phuselab/pyVHR``'s ``cpu_CHROM``) applies :eq:`rppg-chrom` directly
   to *raw* (non-normalized) RGB in the function body actually inspected;
   normalization may happen upstream in that library's own separate
   RGB-extraction stage, which wasn't independently confirmed. This
   implementation applies :eq:`rppg-normalize` first, matching the
   original paper's own stated theoretical requirement for the
   illumination-cancellation argument to hold. If CHROM's output quality
   ever looks systematically wrong in practice, re-verify this specific
   choice against the primary IEEE paper's equations directly before
   assuming a bug elsewhere.

POS: Plane-Orthogonal-to-Skin (default)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~rppg.algorithms.pos_signal` (Wang, den Brinker, Stuijk & de Haan,
IEEE TBME 2017) is generally regarded as the strongest classical rPPG
method, and is this plugin's default backend. Verified directly against a
real, cited reference implementation (``pavisj/rppg-pos``). It projects the
normalized channels through a fixed matrix derived from the skin-tone
plane's orthogonal complement:

.. math::
   :label: rppg-pos

   \begin{bmatrix} X_s \\ Y_s \end{bmatrix}
   =
   \begin{bmatrix} 0 & 1 & -1 \\ -2 & 1 & 1 \end{bmatrix}
   \begin{bmatrix} R_n \\ G_n \\ B_n \end{bmatrix}
   =
   \begin{bmatrix} G_n - B_n \\ -2R_n + G_n + B_n \end{bmatrix}

combined with the same alpha-tuning idiom as CHROM:

.. math::

   \alpha = \frac{\operatorname{std}(X_s)}{\operatorname{std}(Y_s)},
   \qquad
   s = X_s + \alpha \, Y_s

.. note::

   The reference implementation applies this projection over short
   (~1.6 s) overlapping windows with overlap-add reconstruction, tuned for
   real-time streaming use. This implementation applies **one** projection
   per (longer, caller-supplied) HR-analysis window instead, a
   documented, understood simplification of that streaming-specific
   implementation detail, not a misunderstanding of the underlying
   algorithm. Both :eq:`rppg-chrom` and :eq:`rppg-pos` degrade gracefully
   (return an all-zero signal) rather than dividing by zero when a window
   is degenerate (e.g. a frozen/all-black ROI, :math:`\operatorname{std} \approx 0`).

Stage 2: heart-rate extraction
-------------------------------------

Bandpass filtering
~~~~~~~~~~~~~~~~~~~~~~~

:func:`~rppg.hr_estimation.bandpass_filter` applies a zero-phase
(``scipy.signal.filtfilt``) 4th-order Butterworth bandpass restricted to
:math:`[0.7, 3.0]` Hz. Because this also removes DC/slow drift, **no
separate detrending stage is needed**: the bandpass's own low cutoff
subsumes it for windows this short. Too-short input (below
``filtfilt``'s required padding length) degrades gracefully to a
mean-centered, unfiltered copy rather than raising.

Welch-periodogram peak detection
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~rppg.hr_estimation.estimate_hr_welch` computes Welch's periodogram
(``scipy.signal.welch``, one segment spanning the whole window, since
there is no benefit from shorter averaged segments at this scale) and finds
the peak power frequency :math:`f^\star` within the physiological band:

.. math::

   \text{BPM} = 60 \, f^\star

Two refinements keep a short window accurate:

- **Finer than one bin.** A window of :math:`T` seconds resolves frequency
  in steps of :math:`1/T` Hz: 0.1 Hz, i.e. 6 bpm, for the default 10 s
  window, so a 75 bpm pulse would read as 72 or 78. The spectrum is
  zero-padded 8x (``ZERO_PAD``), which interpolates between those steps,
  and the peak is then placed by a parabola through the log-power of the
  peak bin and its two neighbours (exact for a Gaussian-shaped peak, close
  for the Hann window's main lobe). Rates between bins are read to well
  under 1 bpm.
- **Not the harmonic.** The pulse wave is not a sinusoid: its sharp
  systolic upstroke puts power at twice the heart rate, and in some
  windows that harmonic is the stronger peak, which would double the
  reading (150 bpm for a 75 bpm heart). When half the chosen frequency
  still lies in the band and holds a local peak (within 0.1 Hz of exactly
  half) with at least 30 % of the chosen peak's power
  (``SUBHARMONIC_RATIO``) and at least 5 times the band's median power
  (``SUBHARMONIC_FLOOR``), that lower peak is the heart rate. Below a
  genuinely fast heart there is only noise, which rarely passes both tests:
  in simulated windows at an SNR around -15 dB, 1 to 4 % of fast-heart
  windows are halved, which the median smoothing across windows absorbs.
  The reported rate is always kept inside the band.

**Pulse SNR.** The plugin reports a confidence metric in dB: the ratio of
spectral power concentrated at the peak frequency and its first harmonic
versus everything else in the analyzed band (:math:`[0.7, 6.0]` Hz,
:math:`\text{high\_hz} \times 2` extended to catch the harmonic):

.. math::

   \text{SNR}_{\text{dB}} = 10 \log_{10}
       \frac{P_{\text{signal}}}{P_{\text{noise}}}, \qquad
   P_{\text{signal}} = \!\!\sum_{|f - f^\star| \le 0.1
       \text{ or } |f - 2f^\star| \le 0.1}\!\! \text{PSD}(f)

with :math:`P_{\text{noise}} = P_{\text{total}} - P_{\text{signal}}`
(floored at :math:`10^{-12}` to avoid a divide-by-zero). If *every* window
bin has zero power (a fully degenerate, e.g. frozen, signal), the function
returns ``(None, None)`` rather than reporting a meaningless argmax
tie-break as if it were a real peak. This is the same "never fabricate a number
over a real gap" discipline this whole feature is built around.

.. note::

   Unlike :eq:`rppg-chrom`/:eq:`rppg-pos`, this specific SNR formula is a
   standard, documented definition in the spirit of the rPPG literature's
   general "pulse SNR" concept, but is **not** a verified reproduction of
   one single paper's exact formula; no single canonical version was
   independently confirmed during this feature's research pass. Treat it
   as a useful *relative* quality signal (higher is more confident) rather
   than a number with an externally-validated absolute meaning.

Sliding-window mechanics
------------------------------

``run_rppg.py`` slides a window of length ``window_sec`` (default 10 s) by
``hop_sec`` (default 2 s) across the video's *real* per-frame timestamps
(from ``timestamps_camN.csv``, not the video container's nominal frame
rate, since real hardware timing can differ meaningfully from the requested
rate). A window only attempts an estimate if at least 60%
(``MIN_VALID_FRACTION``) of its expected frames had a detected face; below
that, ``bpm``/``snr_db`` are both ``null`` for that window rather than
computed from a sparse, unreliable sample. The **effective sample rate**
used for the bandpass/Welch stages is derived from the real timestamps of
the samples actually present in that window, not assumed from the
video's nominal fps. This is a deliberate, documented simplification versus a
literal interpolate-onto-a-uniform-grid approach, justified by the
density gate already filtering out windows sparse enough for irregular
sampling to matter.

An optional centered median filter (:func:`~rppg.hr_estimation.median_smooth`)
smooths the per-window ``bpm`` series into a separate ``smoothed_bpm``
column for display. The raw column is always kept alongside it, so
nothing is silently overwritten.

Beat-to-beat timing and HRV
------------------------------

The windowed heart rate above finds the dominant frequency of each window.
Heart-rate variability needs every beat. ``rppg/hrv.py`` takes the same
per-frame skin colour through a separate chain. This chain always uses POS,
whatever backend the windows use.

1. **Even sampling.** Frames are placed at their real timestamps and
   resampled at the camera's frame rate. Face gaps up to 0.5 s are bridged;
   longer gaps split the recording into segments of at least 10 s, analysed
   separately.
2. **Continuous pulse wave.** POS runs on overlapping 1.6 s windows that are
   added back together, as in Wang et al. (2017), Algorithm 1. The wave then
   follows slow changes in light and skin tone.
3. **Beats.**

   - The dominant frequency in 0.7 to 3 Hz gives the typical beat
     interval.
   - The wave is band-passed wide (0.5 to 5 Hz, zero-phase), so each beat
     keeps its shape.
   - The wave is upsampled 4x by a cubic spline. Systolic peaks at least
     0.6 of the typical interval apart are found.
   - Each beat is timed at the steepest point of its upstroke, the usual
     fiducial point for pulse waves.
   - Each beat is then re-timed by cross-correlating the stretch around it
     with the person's median beat (a matched filter, 1 ms steps,
     parabolic refinement). This uses the whole beat, not one noisy point.

4. **Cleaning.** Each beat's quality is its shape's correlation with the
   median beat; beats below 0.6 are poor. An interval is **NN**
   (normal-to-normal) only when all of these hold:

   - both its beats are good and in the same segment;
   - it lies between 333 and 1500 ms (40 to 180 bpm);
   - it is within 20% of the median of its 11 neighbours.

5. **HRV** (Task Force, 1996; Shaffer and Ginsberg, 2017), from the NN
   intervals:

   - mean NN and heart rate;
   - **SDNN** (overall variability);
   - **RMSSD** and **pNN50** (beat-to-beat, vagally mediated variability);
   - **LF** (0.04 to 0.15 Hz) and **HF** (0.15 to 0.4 Hz) power, and their
     ratio. These come from the NN series resampled at 4 Hz by cubic spline,
     detrended, with a Welch periodogram over 64 s segments, as Kubios does.
     They need at least 2 minutes.

   RMSSD and heart rate are also given over a sliding minute (10 s steps),
   for windows with at least 30 NN intervals.

HRV, and the sliding-minute series, are **not reported** (the reason is
stated instead) when:

- the frame rate is below 25 fps;
- there are less than 60 s of NN intervals;
- more than 25% of intervals are artifacts.

Timing noise and the split-face correction
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A camera samples the pulse every 20 ms at 50 fps, and sensor noise moves each
detected beat further. Let independent timing noise of :math:`\sigma` affect
every beat. An interval is a difference of two beat times, and a successive
difference of intervals is :math:`b_{k+1} - 2b_k + b_{k-1}`, so

.. math::

   \mathrm{SDNN}^2_{\text{measured}} \approx \mathrm{SDNN}^2 + 2\sigma^2,
   \qquad
   \mathrm{RMSSD}^2_{\text{measured}} \approx \mathrm{RMSSD}^2 + 6\sigma^2.

Camera-based HRV therefore **overestimates** variability, most for people
whose true variability is low. At 15 ms of jitter, noise alone gives an
RMSSD of 37 ms.

The ROI is split along the face's midline (nose tip to below the lower
lip), and the beats are found separately in each half. The beat chain's
"whole face" is the equal-weight average of the two halves (not the pixel
mean of the ROI), so that this holds even when a turned head makes the halves
unequal in size. Both halves see the
same heartbeat, so their beat times differ only by noise and a constant
offset. If each half has noise :math:`s`, the difference has
:math:`\sqrt2\,s`, and the whole face (both halves averaged) has
:math:`s/\sqrt2`. So the whole face's jitter is half the spread of the
differences, taken robustly as 1.4826 times the median absolute deviation
over at least 30 matched beats. The **corrected** values are

.. math::

   \mathrm{RMSSD}_{\text{corrected}} = \sqrt{\max(\mathrm{RMSSD}^2 - 6\sigma^2, 0)},
   \qquad
   \mathrm{SDNN}_{\text{corrected}} = \sqrt{\max(\mathrm{SDNN}^2 - 2\sigma^2, 0)}.

The correction is conservative: noise the two halves share is not seen. That
includes head motion, light flicker and compression artefacts.

On synthetic faces (a pulse with known breathing-linked and random beat
variation, camera noise and drifting light, at 50 fps):

- the true RMSSD was 39 to 41 ms;
- the raw measurement gave 63 to 67 ms;
- the corrected value gave 41 to 51 ms;
- SDNN went from 42 to 46 ms to 32 to 38 ms, against a true 32 to 33 ms.

(Four synthetic recordings, random seeds 0 to 3.)

**This has not been checked against an ECG or a chest strap on a real
face.** Use RMSSD to compare conditions within a person (for example the
same interview's speaking and listening), not as an absolute clinical value.

With Conversation Timing's output, the mean heart rate while the person on
camera speaks and while they listen is also given. Speaking itself raises
heart rate and moves the face, so read differences with that in mind.

.. _rppg-recommendations:

Practical recommendations
------------------------------

.. grid:: 1 1 2 2
   :gutter: 2

   .. grid-item-card:: 🚫  Not a medical device

      Treat every BPM number this plugin produces as a research-grade
      estimate, never a clinical reading. If a real reference measurement
      matters, cross-check against a pulse oximeter or manual pulse count.
      This is explicitly called out because no amount of code review
      substitutes for that comparison.

   .. grid-item-card:: 🧍  Hold still, face the camera

      This is the single biggest accuracy factor. rPPG fundamentally needs
      a face-skin ROI tracked steadily across a whole analysis window. A
      subject who glances away, turns to profile, or moves substantially
      will produce mostly-empty or noisy windows regardless of backend
      choice. A window needs ≥60% face-detected frames just to attempt an
      estimate at all.

   .. grid-item-card:: 💡  Lighting matters more than resolution

      Even, diffuse, reasonably bright lighting on the face gives a much
      cleaner signal than a high-resolution camera in poor or flickering
      light. Avoid strong backlighting and rapidly-changing illumination
      (e.g. a flickering monitor) during the window being analyzed.

   .. grid-item-card:: ⚙️  Which backend to pick

      **POS** (default) is generally the most robust classical method and
      the right starting choice. **CHROM** is a reasonable alternative
      with a similar accuracy profile. **Green** is fast but has no
      motion/illumination compensation, useful mainly as a debugging
      baseline to sanity-check that POS/CHROM are actually doing better,
      not a recommended default.

   .. grid-item-card:: 📊  Reading the quality signal

      Use ``valid_frame_fraction`` and ``snr_db`` together, not BPM alone:
      a plausible-looking BPM from a low-SNR or low-valid-fraction
      window deserves less trust than the same number from a
      high-density, high-SNR one. The in-app quality badge
      (:func:`mosaic::rppg_quality_for`) already combines both signals
      into one Excellent/Good/Acceptable/Poor tier for exactly this
      reason. Treat "Poor" as "don't trust this number," not just a
      cosmetic label.

   .. grid-item-card:: 🎚️  Tuning window/hop length

      A longer ``window_sec`` gives the Welch periodogram finer frequency
      resolution (more cycles observed → a sharper peak) at the cost of
      being less responsive to a genuinely fast heart-rate change and
      needing a longer stretch of continuous good tracking to produce any
      estimate at all. The defaults (10 s window, 2 s hop) are a
      reasonable starting balance. Shorten the window for a
      fast-changing signal you're willing to accept noisier for, lengthen
      it for a resting-state recording where stability matters more than
      responsiveness.

   .. grid-item-card:: 💓  For HRV

      Record in interview mode at 50 fps with even light, and keep at
      least a few minutes. Read the noise-corrected RMSSD, the beat-timing
      noise next to it, and the share of artifacts together. A timing noise
      comparable to the RMSSD itself means the variability is mostly noise.

References (HRV)
------------------------------

- Task Force of the European Society of Cardiology and the North American
  Society of Pacing and Electrophysiology (1996). Heart rate variability:
  standards of measurement, physiological interpretation and clinical use.
  *Circulation* 93(5), 1043 to 1065.
- Shaffer, F. and Ginsberg, J. P. (2017). An overview of heart rate
  variability metrics and norms. *Frontiers in Public Health* 5, 258.
- Wang, W., den Brinker, A. C., Stuijk, S. and de Haan, G. (2017).
  Algorithmic principles of remote PPG. *IEEE Transactions on Biomedical
  Engineering* 64(7), 1479 to 1491.
- Tarvainen, M. P. et al. (2014). Kubios HRV: heart rate variability
  analysis software. *Computer Methods and Programs in Biomedicine* 113(1),
  210 to 220.
