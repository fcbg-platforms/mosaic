"""
Who is speaking: the face on camera (the **subject**) or someone else.

The audio says *that* someone speaks (:mod:`conversation.vad`); the camera
says whether the subject's mouth is moving. Speech moves the jaw and lips
at the syllable rate (3 to 8 per second), so the **mouth activity** used
here is the short-term variability of mouth opening (:func:`mouth_activity`),
not how open the mouth is: a held smile or an open mouth at rest is still.

The threshold for "the mouth moves like speech" is set from the recording
itself: while the audio is silent the subject cannot be speaking, so the
activity then is this person's non-speech movement (:func:`speaking_threshold`).
A conversation with too little silence falls back to splitting the activity
during speech into two groups (Otsu's method), the subject's speech and the
other speaker's.

When the session was diarized (Speaker Diarization plugin), each diarized
speaker label is matched to the subject or not by how much the mouth moves
while that label speaks (:func:`match_diarized_speaker`); the labels then
decide who speaks, and the mouth only fills in where they say nothing.
"""

from __future__ import annotations

import numpy as np

#: Mouth activity window, seconds: a few syllables.
ACTIVITY_WINDOW_S = 0.4
#: The speaking threshold is this percentile of activity during silence.
SILENCE_PERCENTILE = 95.0
#: Silence this close to speech is not used for the threshold, seconds.
SILENCE_MARGIN_S = 0.5
#: Fallback split: the two classes' mean log activity must differ by this
#: much (a factor of about 2.7).
OTSU_MIN_SEPARATION = 1.0
#: Visual speaking: fill gaps and drop bursts shorter than these, seconds.
VIS_FILL_S, VIS_MIN_S = 0.3, 0.2
#: Activity this many times the threshold is speaking even over another voice.
STRONG_FACTOR = 2.0
#: A diarized label is the subject when the mouth moves during this share of
#: its speech, and clearly more than for any other label.
MATCH_MIN, MATCH_MARGIN = 0.35, 0.15
#: Without diarization, speaker switches shorter than this inside continuous
#: speech are treated as mouth-edge noise, seconds.
MIN_SWITCH_S = 0.25
#: Interpolate the mouth over frame gaps up to this long, seconds.
MAX_FRAME_GAP_S = 0.25


def mouth_on_grid(frame_times_s, mouth, grid_s) -> np.ndarray:
    """Mouth opening per frame, interpolated onto the grid (``nan`` where the
    face was not seen, or across longer gaps)."""
    ft = np.asarray(frame_times_s, dtype=np.float64)
    m = np.asarray(mouth, dtype=np.float64)
    g = np.asarray(grid_s, dtype=np.float64)
    ok = np.isfinite(m) & np.isfinite(ft)
    out = np.full(len(g), np.nan)
    if ok.sum() < 2:
        return out
    ft, m = ft[ok], m[ok]
    out = np.interp(g, ft, m, left=np.nan, right=np.nan)
    j = np.clip(np.searchsorted(ft, g), 1, len(ft) - 1)
    gap = ft[j] - ft[j - 1]
    out[gap > MAX_FRAME_GAP_S] = np.nan
    return out


def mouth_activity(mouth_grid, hop_s: float, window_s: float = ACTIVITY_WINDOW_S) -> np.ndarray:
    """Centred rolling standard deviation of mouth opening (``nan`` where
    fewer than half the window has a face)."""
    x = np.asarray(mouth_grid, dtype=np.float64)
    ok = np.isfinite(x)
    w = max(3, int(round(window_s / hop_s)) | 1)
    half = w // 2
    xs = np.where(ok, x, 0.0)
    c1 = np.concatenate([[0.0], np.cumsum(xs)])
    c2 = np.concatenate([[0.0], np.cumsum(xs * xs)])
    cn = np.concatenate([[0], np.cumsum(ok)])
    idx = np.arange(len(x))
    lo = np.clip(idx - half, 0, len(x))
    hi = np.clip(idx + half + 1, 0, len(x))
    n = cn[hi] - cn[lo]
    with np.errstate(divide="ignore", invalid="ignore"):
        mean = (c1[hi] - c1[lo]) / n
        var = (c2[hi] - c2[lo]) / n - mean * mean
    out = np.sqrt(np.clip(var, 0.0, None))
    out[n < w / 2] = np.nan
    return out


def speaking_threshold(activity, speech_mask, hop_s: float = 0.01) -> float | None:
    """Activity above which the mouth moves like speech: a high percentile
    of the activity while nobody speaks. Silence within
    :data:`SILENCE_MARGIN_S` of speech is left out (the activity window
    reaches into the speech). ``None`` without enough silence with the face
    in view (at least 2 s)."""
    a = np.asarray(activity, dtype=np.float64)
    speech = np.asarray(speech_mask, dtype=bool)
    reach = int(round(SILENCE_MARGIN_S / hop_s))
    near = np.convolve(speech.astype(np.int32), np.ones(2 * reach + 1, np.int32), mode="same") > 0
    quiet = ~near & np.isfinite(a)
    if quiet.sum() >= 200:
        return float(np.percentile(a[quiet], SILENCE_PERCENTILE))
    # Too little silence (a dense conversation): split the activity during
    # speech into "mouth moving" and "mouth still" instead.
    talk = speech & np.isfinite(a) & (a > 0)
    if talk.sum() < 200:
        return None
    return otsu_threshold(np.log(a[talk]), separation=OTSU_MIN_SEPARATION, exp=True)


def otsu_threshold(values, separation: float = 0.0, exp: bool = False) -> float | None:
    """Otsu's split of ``values`` into two classes (``exp``: return
    ``exp(threshold)``). ``None`` when the class means are less than
    ``separation`` apart (one class only)."""
    v = np.sort(np.asarray(values, dtype=np.float64))
    n = len(v)
    if n < 2:
        return None
    c = np.cumsum(v)
    k = np.arange(1, n)
    m0 = c[:-1] / k
    m1 = (c[-1] - c[:-1]) / (n - k)
    between = k * (n - k) * (m0 - m1) ** 2
    i = int(np.argmax(between))
    if m1[i] - m0[i] < separation:
        return None
    t = (v[i] + v[i + 1]) / 2.0
    return float(np.exp(t)) if exp else float(t)


def _clean(mask: np.ndarray, hop_s: float) -> np.ndarray:
    from .vad import runs

    m = mask.copy()
    # Brief exceedances go first, so filling cannot join them into speech.
    for value, max_len in ((True, VIS_MIN_S), (False, VIS_FILL_S)):
        for i0, i1 in runs(m, value):
            if (i1 - i0) * hop_s < max_len and (value or (i0 > 0 and i1 < len(m))):
                m[i0:i1] = not value
    return m


def _extend_labels(speech, is_subj, is_other, max_len: int) -> None:
    """Give short unlabelled stretches of speech next to a label that label
    (in place): segment edges and speech edges rarely meet to the frame."""
    from .vad import runs

    has = is_subj | is_other
    for i0, i1 in runs(speech & ~has):
        if i1 - i0 >= max_len:
            continue
        left = i0 - 1 if i0 > 0 and speech[i0 - 1] and has[i0 - 1] else None
        right = i1 if i1 < len(speech) and speech[i1] and has[i1] else None
        src = left if left is not None else right
        if src is None:
            continue
        is_subj[i0:i1] = is_subj[src]
        is_other[i0:i1] = is_other[src]


def _absorb_short_flips(subject: np.ndarray, other: np.ndarray, hop_s: float):
    """Within continuous speech, give runs of one speaker shorter than
    :data:`MIN_SWITCH_S` to the speaker of the longer neighbouring run."""
    from .vad import runs

    label = np.where(subject, 1, np.where(other, 2, 0))
    min_len = int(round(MIN_SWITCH_S / hop_s))
    for _ in range(3):  # a few passes settle chains of short runs
        changed = False
        rr = [(i0, i1, int(label[i0])) for i0, i1 in runs(label > 0)]
        for c0, c1, _ in rr:
            seg = label[c0:c1]
            edges = np.flatnonzero(np.diff(seg)) + 1
            bounds = [0, *edges.tolist(), len(seg)]
            pieces = [(bounds[k], bounds[k + 1]) for k in range(len(bounds) - 1)]
            if len(pieces) < 2:
                continue
            for k, (a, b) in enumerate(pieces):
                if b - a >= min_len:
                    continue
                left = pieces[k - 1] if k > 0 else None
                right = pieces[k + 1] if k + 1 < len(pieces) else None
                nb = max((x for x in (left, right) if x), key=lambda x: x[1] - x[0])
                seg[a:b] = seg[nb[0]]
                changed = True
        if not changed:
            break
    return label == 1, label == 2


def _erode(mask: np.ndarray, n: int) -> np.ndarray:
    """Shrink every true run by ``n`` samples at each end."""
    if n <= 0:
        return mask
    off = np.convolve((~mask).astype(np.int32), np.ones(2 * n + 1, np.int32), mode="same")
    return mask & (off == 0)


def visual_speaking(activity, threshold: float, hop_s: float) -> tuple[np.ndarray, np.ndarray]:
    """``(speaking, strong)`` masks from mouth activity (both false where the
    face is not seen).

    The activity window reaches half its width past each end of a stretch of
    speech, so both masks are shrunk by that much to put their edges back
    where the mouth started and stopped.
    """
    a = np.asarray(activity, dtype=np.float64)
    seen = np.isfinite(a)
    half = int(round(ACTIVITY_WINDOW_S / 2.0 / hop_s))
    above = seen & (np.where(seen, a, 0.0) > threshold)
    speaking = _clean(_erode(above, half), hop_s)
    strong = _erode(seen & (np.where(seen, a, 0.0) > STRONG_FACTOR * threshold), half)
    return speaking & seen, strong


def labels_on_grid(segments, grid_s, offset_s: float = 0.0) -> np.ndarray:
    """Diarized speaker label per grid point (``None`` outside every
    segment). ``segments`` are transcript segments with ``start_ms``,
    ``end_ms`` (audio time) and ``speaker``; ``offset_s`` is the grid time of
    audio time 0."""
    g = np.asarray(grid_s, dtype=np.float64)
    out = np.full(len(g), None, dtype=object)
    for seg in segments:
        if not seg.get("speaker"):
            continue
        a = offset_s + seg["start_ms"] / 1000.0
        b = offset_s + seg["end_ms"] / 1000.0
        out[(g >= a) & (g < b)] = seg["speaker"]
    return out


def label_coverage(segments, grid_s, labels: set) -> np.ndarray:
    """Where any segment with one of ``labels`` covers the grid (segments
    may overlap, so two speakers can both be covered)."""
    g = np.asarray(grid_s, dtype=np.float64)
    out = np.zeros(len(g), bool)
    for seg in segments:
        if seg.get("speaker") in labels:
            out[(g >= seg["start_ms"] / 1000.0) & (g < seg["end_ms"] / 1000.0)] = True
    return out


def match_diarized_speaker(labels, speech_mask, vis_speaking, seen) -> tuple[str | None, dict]:
    """Which diarized label is the subject.

    Returns ``(label or None, scores)``: for each label, the share of its
    speech (with the face in view) during which the mouth moved. ``None``
    unless at least two labels have speech (at least 1 s each) and one of
    them clearly matches the mouth.
    """
    lab = np.asarray(labels, dtype=object)
    sp = np.asarray(speech_mask, bool) & np.asarray(seen, bool)
    vs = np.asarray(vis_speaking, bool)
    scores = {}
    for name in sorted({x for x in lab[sp] if x is not None}):
        sel = sp & (lab == name)
        if sel.sum() >= 100:  # at least 1 s of speech
            scores[name] = round(float(vs[sel].mean()), 3)
    if len(scores) < 2:
        # One label (pyannote merged the voices, or the other said almost
        # nothing): the labels cannot separate the speakers.
        return None, scores
    ranked = sorted(scores.items(), key=lambda kv: -kv[1])
    best, score = ranked[0]
    second = ranked[1][1] if len(ranked) > 1 else 0.0
    if score >= MATCH_MIN and score - second >= MATCH_MARGIN:
        return best, scores
    return None, scores


def attribute(
    speech, seen, vis, strong, subject_cov=None, other_cov=None, hop_s: float = 0.01
) -> dict:
    """Subject and other speech masks.

    With diarization (``subject_cov``/``other_cov``: where the subject's and
    the other labels' segments lie, see :func:`label_coverage`), the labels
    decide, and overlapping segments give overlapping speech; where no label
    covers the speech, the mouth does. The subject is also speaking during
    another label's speech when the mouth moves strongly (overlap). Without
    diarization, speech with the mouth moving is the subject's and speech
    with the mouth still is someone else's, and a switch of speaker lasting
    less than :data:`MIN_SWITCH_S` inside continuous speech is put back to
    the speaker around it (mouth edges are uncertain by about that much);
    overlapping speech is then not detectable. Speech while the face is out
    of view and unlabelled is ``unknown``.
    """
    speech = np.asarray(speech, bool)
    seen = np.asarray(seen, bool)
    vis = np.asarray(vis, bool)
    strong = np.asarray(strong, bool)
    if subject_cov is not None and other_cov is not None:
        is_subj = np.asarray(subject_cov, bool).copy()
        is_other = np.asarray(other_cov, bool).copy()
        _extend_labels(speech, is_subj, is_other, int(round(MIN_SWITCH_S / hop_s)))
        has = is_subj | is_other
        subject = speech & (is_subj | (~has & vis) | (is_other & strong))
        other = speech & (is_other | (~has & seen & ~vis))
    else:
        subject = speech & vis
        other = speech & seen & ~vis
        subject, other = _absorb_short_flips(subject, other, hop_s)
    unknown = speech & ~subject & ~other
    return {"subject": subject, "other": other, "unknown": unknown}
