"""
Keeping one person's identity when the tracker loses them for a moment.

The tracker (BoT-SORT) gives a person a new id when it loses them: someone
walks between them and the camera, they turn away, the detector misses a few
frames. Afterwards the same person, in the same place, carries on under the
new id. :func:`stitch_tracks` joins such pieces after the run: a track that
starts after another has ended, within ``max_gap_s``, and in the same place
(their boxes overlap, compared over their first and last second) continues
that track's identity.

The tracker's own memory of lost tracks is counted in processed frames and
judges by motion only, so a person hidden behind someone passing is often
not recognised again; place is a strong cue for seated participants.
"""

from __future__ import annotations

import numpy as np

#: Join a new track to one that ended at most this long before, seconds.
MAX_GAP_S = 5.0
#: Boxes compared over this long at the end of one track and the start of
#: the next, seconds (a median, so a few frames of the passer do not count).
SETTLE_S = 1.0
#: Minimum overlap (intersection over union) of the two boxes.
MIN_IOU = 0.4


def _iou(a, b) -> float:
    x1, y1 = max(a[0], b[0]), max(a[1], b[1])
    x2, y2 = min(a[2], b[2]), min(a[3], b[3])
    inter = max(0.0, x2 - x1) * max(0.0, y2 - y1)
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / union if union > 0 else 0.0


def stitch_tracks(
    frames: list[dict],
    fps: float = 30.0,
    max_gap_s: float = MAX_GAP_S,
    settle_s: float = SETTLE_S,
    min_iou: float = MIN_IOU,
) -> dict[int, int]:
    """Renumber pieces of one person's track to the id they first had, in
    place. ``frames`` are pose result frames (``timestamp_ns``, else
    ``frame_index`` / ``fps``; ``subjects`` with ``subject_id`` and
    ``bbox_xyxy``). Untracked detections (negative ids) are left alone.
    Returns ``{old id: new id}`` for the ids changed."""
    # One time base per video: the recorded timestamps when every frame has
    # one, else frame_index / fps (mixing the two can step backwards).
    use_ns = all((f.get("timestamp_ns") or 0) > 0 for f in frames)
    t: dict[int, list[float]] = {}
    boxes: dict[int, list[list[float]]] = {}
    for f in frames:
        ts = f["timestamp_ns"] / 1e9 if use_ns else f.get("frame_index", 0) / fps
        for s in f.get("subjects", []):
            sid = s.get("subject_id", -1)
            if sid is None or sid < 0 or len(s.get("bbox_xyxy", [])) != 4:
                continue
            t.setdefault(sid, []).append(ts)
            boxes.setdefault(sid, []).append(s["bbox_xyxy"])
    if len(t) < 2:
        return {}

    first_t, last_t, first_box, last_box = {}, {}, {}, {}
    for sid in t:
        ts = np.asarray(t[sid])
        bb = np.asarray(boxes[sid], dtype=np.float64)
        first_t[sid], last_t[sid] = float(ts.min()), float(ts.max())
        first_box[sid] = np.median(bb[ts <= first_t[sid] + settle_s], axis=0)
        last_box[sid] = np.median(bb[ts >= last_t[sid] - settle_s], axis=0)

    # Every possible join, best overlap first; each track continues at most
    # one and is continued at most once. Taking new tracks in the order they
    # start instead let someone walking past, starting a moment earlier on
    # the same spot, take the hidden person's id.
    order = sorted(t, key=lambda k: first_t[k])
    pairs = []
    for b in order:
        for a in order:
            if a != b and last_t[a] < first_t[b] <= last_t[a] + max_gap_s:
                iou = _iou(last_box[a], first_box[b])
                if iou >= min_iou:
                    pairs.append((iou, a, b))
    pred: dict[int, int] = {}
    continued: set[int] = set()
    for _, a, b in sorted(pairs, key=lambda x: -x[0]):
        if a not in continued and b not in pred:
            pred[b] = a
            continued.add(a)
    root = {}
    for sid in order:  # start order: a predecessor's root is already known
        root[sid] = root[pred[sid]] if sid in pred else sid

    changed = {sid: r for sid, r in root.items() if r != sid}
    if changed:
        for f in frames:
            for s in f.get("subjects", []):
                sid = s.get("subject_id", -1)
                if sid in changed:
                    s["subject_id"] = changed[sid]
    return changed
