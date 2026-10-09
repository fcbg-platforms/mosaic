"""
Turns, transitions, pauses and overlaps from two speakers' speech.

Pure numpy, so every rule is unit-tested. Input: per-hop boolean speech
masks for the **subject** (the face on camera) and the **other** speaker(s)
on a shared time grid; both may be true at once (overlap).

Definitions follow the conversation-analysis literature (Heldner and Edlund,
2010; Levinson and Torreira, 2015):

* A **spurt** is one speaker's speech with silences shorter than
  :data:`MIN_PAUSE_S` filled in.
* A **backchannel** is a short spurt (under :data:`BACKCHANNEL_MAX_S`) mostly
  spoken over the other speaker ("mm-hm", "yeah"): it does not take the
  floor.
* A spurt spoken entirely inside the other speaker's spurt is an **overlap
  without a floor change** (a failed interruption, or a longer
  backchannel): it does not take the floor either.
* Every other spurt belongs to a **turn**: consecutive spurts of one speaker
  make one turn; the silences between them are **pauses**.
* A **transition** is a change of turn. Its **floor transfer offset** (FTO)
  is the next turn's start minus the end of the previous speaker's speech:
  positive is a **gap**, negative an **overlapping transfer**. An overlapping
  transfer that starts at least :data:`INTERRUPTION_S` before the previous
  speaker stops is counted as an **interruption**.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

SUBJECT, OTHER = "subject", "other"
#: Same-speaker silences at least this long are pauses; shorter ones are
#: part of the flow of speech, seconds.
MIN_PAUSE_S = 0.2
#: Backchannels are shorter than this, seconds.
BACKCHANNEL_MAX_S = 1.0
#: ... and at least this share of them is spoken over the other speaker.
BACKCHANNEL_OVERLAP = 0.5
#: An overlapping transfer starting this early is an interruption, seconds.
INTERRUPTION_S = 0.5


@dataclass
class Spurt:
    speaker: str
    start_s: float
    end_s: float
    kind: str = "turn"  # "turn", "backchannel" or "overlap"

    @property
    def duration_s(self) -> float:
        return self.end_s - self.start_s


@dataclass
class Turn:
    speaker: str
    start_s: float
    end_s: float
    spurts: list = field(default_factory=list)
    pauses: list = field(default_factory=list)  # (start_s, end_s)

    @property
    def duration_s(self) -> float:
        return self.end_s - self.start_s

    @property
    def speech_s(self) -> float:
        return sum(s.duration_s for s in self.spurts)


@dataclass
class Transition:
    from_speaker: str
    to_speaker: str
    prev_end_s: float
    next_start_s: float

    @property
    def fto_s(self) -> float:
        return self.next_start_s - self.prev_end_s

    @property
    def interruption(self) -> bool:
        return bool(self.fto_s <= -INTERRUPTION_S)


def mask_intervals(mask, hop_s: float, t0_s: float = 0.0) -> list[tuple[float, float]]:
    """``[start, end)`` times of the true runs of a mask."""
    m = np.asarray(mask, dtype=bool)
    edges = np.flatnonzero(np.diff(np.concatenate([[False], m, [False]]).astype(np.int8)))
    return [
        (t0_s + a * hop_s, t0_s + b * hop_s) for a, b in zip(edges[::2], edges[1::2], strict=True)
    ]


def merge_close(
    intervals: list[tuple[float, float]], max_gap_s: float
) -> list[tuple[float, float]]:
    """Join intervals separated by less than ``max_gap_s``."""
    out: list[list[float]] = []
    for a, b in sorted(intervals):
        if out and a - out[-1][1] < max_gap_s:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return [(a, b) for a, b in out]


def _overlap(a: tuple[float, float], others: list[tuple[float, float]]) -> float:
    return sum(max(0.0, min(a[1], b) - max(a[0], s)) for s, b in others)


def classify_spurts(subject: list, other: list) -> list[Spurt]:
    """All spurts of both speakers, in time order, each marked ``turn``,
    ``backchannel`` or ``overlap`` (see the module docstring)."""
    spurts = []
    for speaker, mine, theirs in ((SUBJECT, subject, other), (OTHER, other, subject)):
        for a, b in mine:
            kind = "turn"
            dur = b - a
            if any(s <= a and b <= e for s, e in theirs):
                kind = "backchannel" if dur < BACKCHANNEL_MAX_S else "overlap"
            elif dur < BACKCHANNEL_MAX_S and _overlap((a, b), theirs) >= BACKCHANNEL_OVERLAP * dur:
                kind = "backchannel"
            spurts.append(Spurt(speaker, a, b, kind))
    return sorted(spurts, key=lambda s: (s.start_s, s.end_s))


def build_turns(spurts: list[Spurt]) -> tuple[list[Turn], list[Transition]]:
    """Turns and the transitions between them, from classified spurts."""
    turns: list[Turn] = []
    transitions: list[Transition] = []
    for s in (s for s in spurts if s.kind == "turn"):
        if turns and turns[-1].speaker == s.speaker:
            t = turns[-1]
            if s.start_s > t.end_s:
                t.pauses.append((t.end_s, s.start_s))
            t.spurts.append(s)
            t.end_s = max(t.end_s, s.end_s)
            continue
        if turns:
            prev = turns[-1]
            # The previous speaker's speech that was under way when this turn began.
            prev_end = max(
                (p.end_s for p in prev.spurts if p.start_s <= s.start_s), default=prev.end_s
            )
            transitions.append(Transition(prev.speaker, s.speaker, prev_end, s.start_s))
        turns.append(Turn(s.speaker, s.start_s, s.end_s, [s]))
    return turns, transitions


def overlap_intervals(
    subject_mask, other_mask, hop_s: float, t0_s: float = 0.0
) -> list[tuple[float, float]]:
    """Times when both speak."""
    both = np.asarray(subject_mask, bool) & np.asarray(other_mask, bool)
    return mask_intervals(both, hop_s, t0_s)


def _stats(values) -> dict:
    v = np.asarray(list(values), dtype=np.float64)
    if v.size == 0:
        return {"n": 0, "median_s": None, "mean_s": None, "q25_s": None, "q75_s": None}
    q25, med, q75 = np.percentile(v, [25, 50, 75])
    return {
        "n": int(v.size),
        "median_s": round(float(med), 3),
        "mean_s": round(float(v.mean()), 3),
        "q25_s": round(float(q25), 3),
        "q75_s": round(float(q75), 3),
    }


def summarise(
    spurts: list[Spurt],
    turns: list[Turn],
    transitions: list[Transition],
    overlaps: list[tuple[float, float]],
    words: dict | None = None,
) -> dict:
    """Per-speaker and conversation numbers.

    ``words`` (optional) maps a speaker role to the number of words the
    transcript gives it, for speech rates.
    """
    if spurts:
        start = min(s.start_s for s in spurts)
        end = max(s.end_s for s in spurts)
    else:
        start = end = 0.0
    span = end - start
    out: dict = {"conversation_s": round(span, 2), "speakers": {}}
    for role in (SUBJECT, OTHER):
        mine = [s for s in spurts if s.speaker == role]
        speech = sum(s.duration_s for s in mine)
        my_turns = [t for t in turns if t.speaker == role]
        turn_time = sum(t.duration_s for t in my_turns)
        pauses = [b - a for t in my_turns for a, b in t.pauses if b - a >= MIN_PAUSE_S]
        responses = [x.fto_s for x in transitions if x.to_speaker == role]
        n_words = (words or {}).get(role)
        out["speakers"][role] = {
            "speech_s": round(speech, 2),
            "speech_pct": round(100.0 * speech / span, 1) if span > 0 else None,
            "turns": len(my_turns),
            "turn_duration": _stats(t.duration_s for t in my_turns),
            "pauses": len(pauses),
            "pauses_per_min": round(60.0 * len(pauses) / turn_time, 2) if turn_time > 0 else None,
            "pause_duration": _stats(pauses),
            "backchannels": sum(1 for s in mine if s.kind == "backchannel"),
            "overlaps_without_floor_change": sum(1 for s in mine if s.kind == "overlap"),
            "response_offset": _stats(responses),
            "interruptions": sum(1 for x in transitions if x.to_speaker == role and x.interruption),
            "words": n_words,
            "words_per_min": round(60.0 * n_words / speech, 1) if n_words and speech > 0 else None,
        }
    ftos = [x.fto_s for x in transitions]
    both = sum(b - a for a, b in overlaps)
    any_speech = sum(b - a for a, b in merge_close([(s.start_s, s.end_s) for s in spurts], 0.0))
    out["transitions"] = {
        "count": len(transitions),
        "floor_transfer_offset": _stats(ftos),
        "gaps": sum(1 for f in ftos if f > 0),
        "overlapping": sum(1 for f in ftos if f <= 0),
        "interruptions": sum(1 for x in transitions if x.interruption),
    }
    out["overlap"] = {
        "count": len(overlaps),
        "total_s": round(both, 2),
        "pct_of_speech": round(100.0 * both / any_speech, 1) if any_speech > 0 else None,
    }
    out["silence_pct"] = round(100.0 * (span - any_speech) / span, 1) if span > 0 else None
    return out
