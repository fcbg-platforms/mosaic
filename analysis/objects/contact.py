"""Objects as outlines, and when a hand touches them.

Every object a frame shows is kept as its outline (a simplified polygon of its
mask), so the result stays small and contact can be tested against the real
shape, not only its box. A hand "touches" an object when a wrist keypoint
(COCO 9 and 10, from the Pose plugin) lies inside the outline or within a
margin of it: the wrist sits a little back from the fingertips, which are what
actually touch.
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


def margin_for(polygon: list) -> float:
    if len(polygon) < 3:
        return MIN_MARGIN_PX
    p = np.asarray(polygon, float)
    size = float(np.sqrt(max(1.0, (np.ptp(p[:, 0]) + 1) * (np.ptp(p[:, 1]) + 1))))
    return max(MIN_MARGIN_PX, MARGIN_SHARE * size)


def frame_contacts(objects: list, people: list) -> list:
    """Which person's wrist touches which object in one frame.

    Parameters
    ----------
    objects : list of dict
        ``{"id", "polygon"}`` per object.
    people : list of dict
        Pose subjects: ``{"subject_id", "keypoints", "visibilities"}``.

    Returns
    -------
    list of (object id, person id)
    """
    out = []
    for o in objects:
        poly = o.get("polygon") or []
        margin = margin_for(poly)
        for p in people:
            kp = np.asarray(p.get("keypoints") or [], float).reshape(-1, 2)
            vis = np.asarray(p.get("visibilities") or [], float)
            for w in WRISTS:
                if w < len(kp) and w < len(vis) and vis[w] > 0.5 and touches(poly, kp[w], margin):
                    out.append((o["id"], p.get("subject_id")))
                    break
    return out


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
    """A Pose subject id as text; ``?`` for a person Pose did not track."""
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
