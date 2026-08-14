Remote Heart Rate (rPPG)
============================

.. contents:: On this page
   :local:
   :depth: 2

.. important::

   **EXPERIMENTAL.** This plugin is a research-grade heart-rate estimate
   only — it is **not a medical device** and has **not been clinically
   validated**. Camera-based pulse-rate estimation (remote
   photoplethysmography, "rPPG") is a real, published technique with known
   accuracy bounds under good conditions, but it is easily degraded by
   motion, lighting, and compression artifacts. See
   :ref:`rppg-recommendations` below before trusting any output number.

Implemented in :mod:`rppg.roi` (face-skin ROI extraction),
:mod:`rppg.algorithms` (pulse-signal combination), and
:mod:`rppg.hr_estimation` (frequency-domain heart-rate extraction) — see
:doc:`/analysis_api` for the full API reference. This page derives the
physiological signal model and all three classical (non-deep-learning)
combination algorithms the plugin offers, plus the windowed Welch-periodogram
estimation that turns a pulse signal into a BPM number.

The physiological signal
-----------------------------

Remote photoplethysmography exploits a real, physical fact: as the heart
beats, blood volume in facial skin capillaries oscillates at the pulse
rate, producing a tiny, periodic change in how much light the skin
reflects — strongest in the green channel, since haemoglobin absorbs green
light more than red. The whole plugin's job is to recover this weak
periodic signal from an ROI's mean RGB values, which are otherwise
dominated by illumination, motion, and camera-noise variation orders of
magnitude larger than the pulse signal itself.

The physiological pulse band searched throughout is fixed at
:math:`[0.7, 3.0]` Hz — 42 to 180 BPM, covering adult resting through
moderate-exertion heart rate (``LOW_HZ``/``HIGH_HZ`` in ``run_rppg.py``).

ROI extraction
-------------------

:class:`~rppg.roi.MediaPipeFaceRoiExtractor` uses MediaPipe FaceLandmarker
to locate a fixed 9-point **lower-face** polygon (both cheeks plus the
region between them — deliberately avoiding eyes, eyebrows, and the mouth,
which move independently of the pulse signal via blinking/speech) and
computes the mean RGB value inside it for every processed frame:

.. math::

   \bar{c} = \frac{1}{|\Omega|} \sum_{(x,y) \in \Omega} I_c(x, y),
   \qquad c \in \{R, G, B\}

where :math:`\Omega` is the ROI polygon's pixel mask. The landmark indices
are a real, cited reference (SamProell/yarppg's ``FaceMeshDetector``,
itself attributed to Li, Chen, Zhao & Pietikäinen, CVPR 2014) — verified
against a real implementation before being used, not invented from memory,
matching this project's established discipline for algorithmic claims. A
frame with no detected face contributes **no** sample at all — this is a
real, honest gap, never interpolated or fabricated.

Stage 1 — pulse-signal combination
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

Green — naive baseline
~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~rppg.algorithms.green_signal` (Verkruysse, Svaasand & Nelson, 2008)
is simply the mean-centered raw green channel:

.. math::

   s = G - \bar{G}

No motion or illumination compensation at all — the original, simplest rPPG
method, kept as a fast baseline for comparison/debugging rather than the
default, since any camera or subject motion during the window directly
corrupts it.

CHROM — chrominance-based
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~rppg.algorithms.chrom_signal` (de Haan & Jeanne, IEEE TBME 2013)
builds two **chrominance** signals from the temporally-normalized channels
:math:`(R_n, G_n, B_n)`:

.. math::
   :label: rppg-chrom

   X_c = 3 R_n - 2 G_n, \qquad Y_c = 1.5 R_n + G_n - 1.5 B_n

derived from a skin-reflection model in which specular/illumination changes
affect :math:`X_c` and :math:`Y_c` proportionally, while the pulse
component does not — so a single alpha-tuned combination cancels most of
the shared illumination component while preserving the pulse signal:

.. math::

   \alpha = \frac{\operatorname{std}(X_c)}{\operatorname{std}(Y_c)},
   \qquad
   s = X_c - \alpha \, Y_c

.. note::

   **A real ambiguity, surfaced rather than silently resolved.** A
   reference implementation consulted while verifying this formula
   (``phuselab/pyVHR``'s ``cpu_CHROM``) applies :eq:`rppg-chrom` directly
   to *raw* (non-normalized) RGB in the function body actually inspected —
   normalization may happen upstream in that library's own separate
   RGB-extraction stage, which wasn't independently confirmed. This
   implementation applies :eq:`rppg-normalize` first, matching the
   original paper's own stated theoretical requirement for the
   illumination-cancellation argument to hold. If CHROM's output quality
   ever looks systematically wrong in practice, re-verify this specific
   choice against the primary IEEE paper's equations directly before
   assuming a bug elsewhere.

POS — Plane-Orthogonal-to-Skin (default)
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
   per (longer, caller-supplied) HR-analysis window instead — a
   documented, understood simplification of that streaming-specific
   implementation detail, not a misunderstanding of the underlying
   algorithm. Both :eq:`rppg-chrom` and :eq:`rppg-pos` degrade gracefully
   (return an all-zero signal) rather than dividing by zero when a window
   is degenerate (e.g. a frozen/all-black ROI, :math:`\operatorname{std} \approx 0`).

Stage 2 — heart-rate extraction
-------------------------------------

Bandpass filtering
~~~~~~~~~~~~~~~~~~~~~~~

:func:`~rppg.hr_estimation.bandpass_filter` applies a zero-phase
(``scipy.signal.filtfilt``) 4th-order Butterworth bandpass restricted to
:math:`[0.7, 3.0]` Hz. Because this also removes DC/slow drift, **no
separate detrending stage is needed** — the bandpass's own low cutoff
subsumes it for windows this short. Too-short input (below
``filtfilt``'s required padding length) degrades gracefully to a
mean-centered, unfiltered copy rather than raising.

Welch-periodogram peak detection
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:func:`~rppg.hr_estimation.estimate_hr_welch` computes Welch's periodogram
(``scipy.signal.welch``, one segment spanning the whole window — no benefit
from shorter averaged segments at this scale) and finds the peak power
frequency :math:`f^\star` within the physiological band:

.. math::

   \text{BPM} = 60 \, f^\star

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
tie-break as if it were a real peak — the same "never fabricate a number
over a real gap" discipline this whole feature is built around.

.. note::

   Unlike :eq:`rppg-chrom`/:eq:`rppg-pos`, this specific SNR formula is a
   standard, documented definition in the spirit of the rPPG literature's
   general "pulse SNR" concept, but is **not** a verified reproduction of
   one single paper's exact formula — no single canonical version was
   independently confirmed during this feature's research pass. Treat it
   as a useful *relative* quality signal (higher is more confident) rather
   than a number with an externally-validated absolute meaning.

Sliding-window mechanics
------------------------------

``run_rppg.py`` slides a window of length ``window_sec`` (default 10 s) by
``hop_sec`` (default 2 s) across the video's *real* per-frame timestamps
(from ``timestamps_camN.csv``, not the video container's nominal frame
rate — real hardware timing can differ meaningfully from the requested
rate). A window only attempts an estimate if at least 60%
(``MIN_VALID_FRACTION``) of its expected frames had a detected face; below
that, ``bpm``/``snr_db`` are both ``null`` for that window rather than
computed from a sparse, unreliable sample. The **effective sample rate**
used for the bandpass/Welch stages is derived from the real timestamps of
the samples actually present in that window, not assumed from the
video's nominal fps — a deliberate, documented simplification versus a
literal interpolate-onto-a-uniform-grid approach, justified by the
density gate already filtering out windows sparse enough for irregular
sampling to matter.

An optional centered median filter (:func:`~rppg.hr_estimation.median_smooth`)
smooths the per-window ``bpm`` series into a separate ``smoothed_bpm``
column for display — the raw column is always kept alongside it, so
nothing is silently overwritten.

.. _rppg-recommendations:

Practical recommendations
------------------------------

.. grid:: 1 1 2 2
   :gutter: 2

   .. grid-item-card:: 🚫  Not a medical device

      Treat every BPM number this plugin produces as a research-grade
      estimate, never a clinical reading. If a real reference measurement
      matters, cross-check against a pulse oximeter or manual pulse count
      — this is explicitly called out because no amount of code review
      substitutes for that comparison.

   .. grid-item-card:: 🧍  Hold still, face the camera

      This is the single biggest accuracy factor. rPPG fundamentally needs
      a face-skin ROI tracked steadily across a whole analysis window — a
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
      motion/illumination compensation — useful mainly as a debugging
      baseline to sanity-check that POS/CHROM are actually doing better,
      not a recommended default.

   .. grid-item-card:: 📊  Reading the quality signal

      Use ``valid_frame_fraction`` and ``snr_db`` together, not BPM alone
      — a plausible-looking BPM from a low-SNR or low-valid-fraction
      window deserves less trust than the same number from a
      high-density, high-SNR one. The in-app quality badge
      (:func:`mosaic::rppg_quality_for`) already combines both signals
      into one Excellent/Good/Acceptable/Poor tier for exactly this
      reason — treat "Poor" as "don't trust this number," not just a
      cosmetic label.

   .. grid-item-card:: 🎚️  Tuning window/hop length

      A longer ``window_sec`` gives the Welch periodogram finer frequency
      resolution (more cycles observed → a sharper peak) at the cost of
      being less responsive to a genuinely fast heart-rate change and
      needing a longer stretch of continuous good tracking to produce any
      estimate at all. The defaults (10 s window, 2 s hop) are a
      reasonable starting balance — shorten the window for a
      fast-changing signal you're willing to accept noisier for, lengthen
      it for a resting-state recording where stability matters more than
      responsiveness.
