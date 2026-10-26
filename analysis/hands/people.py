"""Whose hand, which side, and when each hand is seen: plain geometry on the
points the models give, no model needed (so it is tested on its own)."""

from __future__ import annotations

import numpy as np

#: The 21 hand points, in the order both models give them (MediaPipe's, and
#: COCO-WholeBody's hands, which RTMW and the RTMPose hand model use).
KEYPOINT_NAMES = [
    "wrist",
    "thumb_cmc",
    "thumb_mcp",
    "thumb_ip",
    "thumb_tip",
    "index_mcp",
    "index_pip",
    "index_dip",
    "index_tip",
    "middle_mcp",
    "middle_pip",
    "middle_dip",
    "middle_tip",
    "ring_mcp",
    "ring_pip",
    "ring_dip",
    "ring_tip",
    "pinky_mcp",
    "pinky_pip",
    "pinky_dip",
    "pinky_tip",
]

#: Lines between the points, for drawing.
EDGES = [
    (0, 1), (1, 2), (2, 3), (3, 4),
    (0, 5), (5, 6), (6, 7), (7, 8),
    (5, 9), (9, 10), (10, 11), (11, 12),
    (9, 13), (13, 14), (14, 15), (15, 16),
    (13, 17), (0, 17), (17, 18), (18, 19), (19, 20),
]  # fmt: skip

#: COCO body points (Pose): each side's elbow and wrist.
ARM = {"left": (7, 9), "right": (8, 10)}
#: A Pose keypoint below this visibility is not used.
MIN_VISIBILITY = 0.3


def hand_size(xy) -> float:
    """The hand's extent in pixels: the longer side of its points' box."""
    a = np.asarray(xy, float)
    return float(np.ptp(a, axis=0).max()) if len(a) else 0.0


def wrists(person: dict) -> dict:
    """``{"left": (x, y), "right": (x, y)}``, the person's seen wrists."""
    kp = person.get("keypoints") or []
    vis = person.get("visibilities") or [1.0] * len(kp)
    out = {}
    for side, (_elbow, wrist) in ARM.items():
        if wrist < len(kp) and wrist < len(vis) and vis[wrist] >= MIN_VISIBILITY:
            out[side] = np.asarray(kp[wrist], float)
    return out


def hand_seeds(people: list) -> list:
    """Where to look for hands: a little past each seen wrist along the
    forearm, with a size from the forearm's length. ``[(centre, size)]``."""
    seeds = []
    for p in people:
        kp = np.asarray(p.get("keypoints") or [], float)
        vis = p.get("visibilities") or [1.0] * len(kp)
        for elbow, wrist in ARM.values():
            if wrist >= len(kp) or wrist >= len(vis) or vis[wrist] < MIN_VISIBILITY:
                continue
            seen = elbow < len(vis) and vis[elbow] >= MIN_VISIBILITY
            fore = kp[wrist] - kp[elbow] if seen else np.zeros(2)
            length = float(np.linalg.norm(fore))
            size = float(np.clip(0.8 * length, 120.0, 400.0)) if length > 0 else 200.0
            seeds.append((kp[wrist] + 0.35 * fore, size))
    return seeds


def assign_by_wrist(hands: list, people: list) -> list:
    """Gives each hand found without a person (MediaPipe) the person and side
    of the nearest seen wrist, nearest pairs first, each wrist once; a hand
    no wrist is near (within its own size) keeps ``subject_id`` None and
    ``side`` "unknown". MediaPipe's own left/right is not used: it flips
    from frame to frame on hands seen from the side."""
    pairs = []
    for i, h in enumerate(hands):
        w0 = np.asarray(h["keypoints"][0], float)
        reach = max(hand_size(h["keypoints"]), 60.0)
        for p in people:
            for side, w in wrists(p).items():
                d = float(np.linalg.norm(w - w0))
                if d <= reach:
                    pairs.append((d, i, p.get("subject_id"), side))
    used_hands, used_wrists = set(), set()
    for h in hands:
        h["subject_id"], h["side"] = None, "unknown"
    for _d, i, pid, side in sorted(pairs, key=lambda t: t[0]):
        if i in used_hands or (pid, side) in used_wrists:
            continue
        hands[i]["subject_id"], hands[i]["side"] = pid, side
        used_hands.add(i)
        used_wrists.add((pid, side))
    return hands


def _same_hand(a: dict, b: dict) -> bool:
    """Whether two hands found are one: centres closer than 30% of the
    larger, and nothing says they are different hands (another side, or
    another person: two people's right hands meet in a handshake)."""
    for key in ("side", "subject_id"):
        if a.get(key) is not None and b.get(key) is not None and a[key] != b[key]:
            return False
    gap = np.linalg.norm(
        np.asarray(a["keypoints"], float).mean(axis=0)
        - np.asarray(b["keypoints"], float).mean(axis=0)
    )
    return bool(gap <= 0.3 * max(hand_size(a["keypoints"]), hand_size(b["keypoints"])))


def drop_repeats(hands: list) -> list:
    """One hand where two were found on top of each other (two seeds on one
    hand): the better score is kept. A person's two hands holding one ball
    are both kept, as are two people's hands meeting."""
    kept = []
    for h in sorted(hands, key=lambda h: -h["score"]):
        if not any(_same_hand(h, k) for k in kept):
            kept.append(h)
    return kept


def box_overlap(a, b) -> float:
    """Intersection over union of two ``[x0, y0, x1, y1]`` boxes."""
    w = max(0.0, min(a[2], b[2]) - max(a[0], b[0]))
    h = max(0.0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = w * h
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / union if union > 0 else 0.0


def distinct_people(people: list) -> list:
    """The people with a box, one per person: of two boxes overlapping by
    more than half (Pose found one person twice), the more confident."""
    kept = []
    with_box = [p for p in people if p.get("bbox_xyxy")]
    for p in sorted(with_box, key=lambda p: -float(p.get("confidence", 0.0))):
        if all(box_overlap(p["bbox_xyxy"], k["bbox_xyxy"]) <= 0.5 for k in kept):
            kept.append(p)
    return kept


def seen_spans(times_ms: list, max_gap_ms: float) -> list:
    """``[[start, end], ...]`` runs of the given times, a run ending where
    the next time is more than ``max_gap_ms`` later."""
    spans = []
    for t in times_ms:
        if spans and t - spans[-1][1] <= max_gap_ms:
            spans[-1][1] = t
        else:
            spans.append([t, t])
    return spans


def summarise(frames: list, step_ms: float) -> list:
    """One entry per (person, side): how long the hand was seen and when,
    its median size in pixels and its mean score."""
    found: dict = {}
    for f in frames:
        for h in f["hands"]:
            key = (h["subject_id"], h["side"])
            entry = found.setdefault(key, {"times": [], "sizes": [], "scores": []})
            entry["times"].append(f["timestamp_ms"])
            entry["sizes"].append(hand_size(h["keypoints"]))
            entry["scores"].append(h["score"])
    out = []
    for (pid, side), e in sorted(found.items(), key=lambda kv: (str(kv[0][0]), kv[0][1])):
        spans = seen_spans(e["times"], max(500.0, 2.5 * step_ms))
        out.append(
            {
                "subject_id": pid,
                "side": side,
                "frames": len(e["times"]),
                "seen_ms": round(len(e["times"]) * step_ms, 1),
                "seen_spans": [[round(a, 1), round(b + step_ms, 1)] for a, b in spans],
                "median_size_px": round(float(np.median(e["sizes"])), 1),
                "mean_score": round(float(np.mean(e["scores"])), 3),
            }
        )
    return out
