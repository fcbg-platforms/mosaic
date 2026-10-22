"""Objects as outlines, and when a hand touches them.

Every object a frame shows is kept as its outline (a simplified polygon of its
mask), so the result stays small and contact can be tested against the real
shape, not only its box. A hand "touches" an object when its own outline
(SAM 3 follows hands too) meets the object's, or when a wrist keypoint (COCO 9
and 10, from the Pose plugin) lies inside the outline or within a margin of
it: the wrist sits a little back from the fingertips, which are what actually
touch. The hand finds touches on close-up cameras, where Pose sees no person;
Pose says whose hand it is.
"""

from __future__ import annotations

import cv2
import numpy as np

#: COCO-17 wrists.
WRISTS = (9, 10)
#: Wrists closer to an outline than this share of the object's size count as
#: touching (fingers reach beyond the wrist keypoint).
MARGIN_SHARE = 0.25
MIN_MARGIN_PX = 12.0
#: SAM 3 follows hands under this name, next to the objects; a hand is not
#: an object of the results.
HAND = "hand"
#: A hand closer to an outline than this share of the hand's size counts as
#: touching it: the two masks meet at the fingers, and simplifying both
#: outlines moves their edges by a few pixels.
HAND_MARGIN_SHARE = 0.05
MIN_HAND_MARGIN_PX = 3.0
#: Contacts shorter than this are noise; gaps shorter than that are bridged.
MIN_CONTACT_S = 0.2
BRIDGE_GAP_S = 0.3


def outline(mask: np.ndarray, max_points: int = 64) -> list:
    """The largest region of a boolean mask as a simplified polygon,
    ``[[x, y], ...]`` in pixels; empty for an empty mask."""
    m = np.ascontiguousarray(mask.astype(np.uint8))
    contours, _ = cv2.findContours(m, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return []
    c = max(contours, key=cv2.contourArea)
    eps = 0.005 * cv2.arcLength(c, True)
    while True:
        poly = cv2.approxPolyDP(c, eps, True).reshape(-1, 2)
        if len(poly) <= max_points:
            return poly.astype(int).tolist()
        eps *= 1.5


def touches(polygon: list, point, margin_px: float) -> bool:
    """Whether ``point`` lies inside ``polygon`` or within ``margin_px`` of it."""
    if len(polygon) < 3:
        return False
    poly = np.asarray(polygon, np.float32).reshape(-1, 1, 2)
    d = cv2.pointPolygonTest(poly, (float(point[0]), float(point[1])), True)
    return d >= -margin_px


def size_of(polygon: list) -> float:
    """The square root of the outline's bounding box area, pixels."""
    p = np.asarray(polygon, float)
    return float(np.sqrt(max(1.0, (np.ptp(p[:, 0]) + 1) * (np.ptp(p[:, 1]) + 1))))


def margin_for(polygon: list) -> float:
    if len(polygon) < 3:
        return MIN_MARGIN_PX
    return max(MIN_MARGIN_PX, MARGIN_SHARE * size_of(polygon))


def outlines_meet(a: list, b: list, margin_px: float) -> bool:
    """Whether two outlines overlap or come within ``margin_px`` of each other.

    Both are drawn into a small mask around them, ``a`` grown by the margin:
    testing only the vertices would miss two edges that cross between them.
    """
    if len(a) < 3 or len(b) < 3:
        return False
    pa, pb = np.asarray(a, np.int32), np.asarray(b, np.int32)
    gap = np.maximum(pa.min(axis=0) - pb.max(axis=0), pb.min(axis=0) - pa.max(axis=0))
    if gap.max() > margin_px:  # their boxes are already too far apart
        return False
    r = int(np.ceil(margin_px))
    lo = np.minimum(pa.min(axis=0), pb.min(axis=0)) - r - 1
    hi = np.maximum(pa.max(axis=0), pb.max(axis=0)) + r + 1
    ma = np.zeros((hi[1] - lo[1] + 1, hi[0] - lo[0] + 1), np.uint8)
    mb = np.zeros_like(ma)
    cv2.fillPoly(ma, [(pa - lo).reshape(-1, 1, 2)], 1)
    cv2.fillPoly(mb, [(pb - lo).reshape(-1, 1, 2)], 1)
    if r > 0:
        ma = cv2.dilate(ma, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1, 2 * r + 1)))
    return bool((ma & mb).any())


def _seen_wrists(person: dict) -> list:
    kp = np.asarray(person.get("keypoints") or [], float).reshape(-1, 2)
    vis = np.asarray(person.get("visibilities") or [], float)
    return [kp[w] for w in WRISTS if w < len(kp) and w < len(vis) and vis[w] > 0.5]


def hand_owner(hand: list, people: list):
    """The Pose subject whose seen wrist is on or nearest this hand's outline,
    within the wrist margin; ``None`` when no wrist is (Pose found no one, or
    only an arm is in view)."""
    best, best_d = None, None
    if len(hand) < 3:
        return None
    poly = np.asarray(hand, np.float32).reshape(-1, 1, 2)
    margin = margin_for(hand)
    for p in people:
        for w in _seen_wrists(p):
            d = -cv2.pointPolygonTest(poly, (float(w[0]), float(w[1])), True)
            if d <= margin and (best_d is None or d < best_d):
                best, best_d = p.get("subject_id"), d
    return best


def frame_contacts(objects: list, people: list, hands: list = ()) -> list:
    """Which person's hand touches which object in one frame.

    Parameters
    ----------
    objects : list of dict
        ``{"id", "polygon"}`` per object.
    people : list of dict
        Pose subjects: ``{"subject_id", "keypoints", "visibilities"}``.
    hands : list of dict
        The hands SAM 3 found, ``{"polygon"}`` each.

    Returns
    -------
    list of (object id, person id), the person ``None`` for a hand Pose did
    not place; such a pair is left out when a known person touches the same
    object in that frame (the same touch, seen both ways).
    """
    out = []
    for o in objects:
        poly = o.get("polygon") or []
        margin = margin_for(poly)
        for p in people:
            if any(touches(poly, w, margin) for w in _seen_wrists(p)):
                out.append((o["id"], p.get("subject_id")))
    for h in hands:
        hand = h.get("polygon") or []
        if len(hand) < 3:
            continue
        margin = max(MIN_HAND_MARGIN_PX, HAND_MARGIN_SHARE * size_of(hand))
        met = [o["id"] for o in objects if outlines_meet(hand, o.get("polygon") or [], margin)]
        if met:
            owner = hand_owner(hand, people)
            out.extend((oid, owner) for oid in met)
    known = {obj for obj, person in out if person is not None}
    out = [(obj, person) for obj, person in out if person is not None or obj not in known]
    return list(dict.fromkeys(out))


def _runs(times_ms: list, step_ms: float) -> list:
    """Sample times (sorted) as ``[start, end]`` runs: samples closer than
    the bridging gap join, and each run lasts to its last sample plus one
    step (the stretch that sample stands for)."""
    gap = max(BRIDGE_GAP_S * 1000.0, 1.5 * step_ms)
    runs: list = []
    for t in times_ms:
        if runs and t - runs[-1][1] <= gap:
            runs[-1][1] = t
        else:
            runs.append([t, t])
    return [[a, b + step_ms] for a, b in runs]


def person_key(person) -> str:
    """A Pose subject id as text; ``?`` for a person Pose did not track or a
    hand it did not place."""
    return "?" if person is None else str(person)


def contact_events(per_frame: list, times_ms: list, step_ms: float = 0.0) -> list:
    """Contact runs from per-frame (object, person) pairs.

    ``per_frame[k]`` is the list of pairs in the analysed frame at
    ``times_ms[k]``; ``step_ms`` is the time between analysed frames. Samples
    of the same pair closer than ``BRIDGE_GAP_S`` (or one and a half steps)
    are joined; runs shorter than ``MIN_CONTACT_S`` are dropped. Returns
    ``{"object_id", "person", "start_ms", "end_ms"}`` in time order.
    """
    times: dict = {}
    for pairs, t in zip(per_frame, times_ms, strict=False):
        for pair in set(pairs):
            times.setdefault(pair, []).append(t)
    out = []
    for (obj, person), ts in times.items():
        for a, b in _runs(ts, step_ms):
            if b - a >= MIN_CONTACT_S * 1000.0:
                out.append({"object_id": obj, "person": person, "start_ms": a, "end_ms": b})
    return sorted(out, key=lambda e: (e["start_ms"], e["object_id"]))


def summarise(frames: list, events: list, step_ms: float = 0.0) -> list:
    """Per object: label, share of analysed frames it was seen in, when
    (``seen_spans``, ms), its mean area and how far its centre travelled (px),
    and its contacts. An object whose mask came back empty in a frame does
    not count as seen there."""
    by_id: dict = {}
    n = len(frames)
    for f in frames:
        for o in f["objects"]:
            poly = np.asarray(o.get("polygon") or [], float)
            if len(poly) < 3:
                continue
            s = by_id.setdefault(
                o["id"],
                {"id": o["id"], "label": o["label"], "frames": 0, "area": 0.0, "path": [], "t": []},
            )
            s["frames"] += 1
            s["area"] += float(cv2.contourArea(poly.astype(np.float32)))
            s["path"].append(poly.mean(axis=0))
            s["t"].append(f["timestamp_ms"])
    out = []
    for oid, s in sorted(by_id.items()):
        path = np.asarray(s["path"], float)
        travel = (
            float(np.linalg.norm(np.diff(path, axis=0), axis=1).sum()) if len(path) > 1 else 0.0
        )
        mine = [e for e in events if e["object_id"] == oid]
        by_person: dict = {}
        for e in mine:
            k = person_key(e["person"])
            by_person[k] = round(by_person.get(k, 0.0) + (e["end_ms"] - e["start_ms"]) / 1000.0, 2)
        out.append(
            {
                "id": oid,
                "label": s["label"],
                "seen_pct": round(100.0 * s["frames"] / n, 1) if n else 0.0,
                "first_ms": s["t"][0],
                "last_ms": s["t"][-1],
                "seen_spans": [[round(a, 1), round(b, 1)] for a, b in _runs(s["t"], step_ms)],
                "mean_area_px": round(s["area"] / s["frames"], 1),
                "travel_px": round(travel, 1),
                "contacts": len(mine),
                "touched_s_by_person": by_person,
            }
        )
    return out
