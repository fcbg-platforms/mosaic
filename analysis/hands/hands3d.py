"""3D hands: the hands two or more calibrated cameras see, triangulated into
the room (millimetres), each kept as one hand over time, and how well the
cameras agree. See docs/math/hands.rst.

Each camera's Hands result gives the hands it saw, with the person's own
side; the cameras are lined up by the session's trigger ticks
(``sync_manifest.json``), as in 3D Pose. Pose numbers people on each camera
on its own, so hands are matched across cameras by side and by how well
their points agree: the two views whose triangulated points reproject
closest to what each camera saw, if close enough.
"""

from __future__ import annotations

import csv
from itertools import combinations
from pathlib import Path

import numpy as np
from pose3d.triangulation import (
    CameraGeom,
    normalize_point,
    project_point_px,
    projection_matrix,
    triangulate_point_dlt,
    triangulate_with_rejection,
)

#: Two cameras' views of a hand are one hand when their points agree within
#: this share of the hand's size in pixels (about 40 px for a 170 px hand)...
AGREEMENT_SHARE = 0.25
#: ...or this many pixels, when that is more.
MIN_AGREEMENT_PX = 20.0
#: Points both views must have (scored at least this) to judge agreement.
MIN_SHARED_POINTS = 8
MIN_POINT_SCORE = 0.2
#: A hand found again is the same hand when its wrist is within this...
TRACK_JUMP_MM = 150.0
#: ...plus this speed times the time since it was last seen (a hand moves
#: at most about 2 m/s), and that time is at most this.
TRACK_SPEED_MM_S = 2000.0
TRACK_GAP_S = 2.0
#: Two cameras' frames at most this far apart in time count as taken at
#: once, for the calibration's part of their disagreement...
SAME_MOMENT_MS = 10.0
#: ...when there are at least this many such hands.
MIN_SAME_MOMENT_HANDS = 20


def camera_geoms(meta: dict) -> dict:
    """``{camera index: CameraGeom}`` for the cameras with both lens and room
    calibration in ``session_meta.json``."""
    out = {}
    for c in meta.get("cameras", []):
        cal = c.get("calibration") or {}
        if cal.get("calibrated") and cal.get("extrinsic_calibrated") and cal.get("extrinsic_rt"):
            out[c["index"]] = CameraGeom(
                index=c["index"],
                camera_matrix=np.array(cal["camera_matrix"], float).reshape(3, 3),
                dist_coeffs=np.array(cal["dist_coeffs"], float).ravel(),
                extrinsic_rt=np.array(cal["extrinsic_rt"], float).reshape(4, 4),
            )
    return out


class CameraHands:
    """One camera's Hands frames, found by the hardware frame id a tick
    names (``timestamps_camN.csv`` maps it to the decoded frame; without
    that file, frame id N is decoded frame N - 1, as in 3D Pose)."""

    def __init__(self, frames: list, timestamps_csv: Path):
        self.by_decode = {f["frame_index"]: f["hands"] for f in frames}
        self.decode_of_id = None
        if timestamps_csv.exists():
            self.decode_of_id = {}
            with timestamps_csv.open() as f:
                for i, row in enumerate(csv.DictReader(f)):
                    try:
                        self.decode_of_id[int(row["frame_id"])] = i
                    except (KeyError, ValueError):
                        pass

    def at_frame_id(self, frame_id: int) -> list | None:
        """The hands of a known side in that frame; None when the frame was
        not analysed (every N frames) or is not in the timestamps: a tick
        is put in 3D only from frames each camera really took then."""
        if self.decode_of_id is None:
            decode = frame_id - 1
        else:
            decode = self.decode_of_id.get(frame_id)
        if decode is None or decode not in self.by_decode:
            return None
        return [h for h in self.by_decode[decode] if h["side"] != "unknown"]


def hand_size_px(hand: dict) -> float:
    """The hand's extent in pixels, over its points scored at least
    :data:`MIN_POINT_SCORE` (a misplaced weak point would inflate it)."""
    pts = np.asarray(hand["keypoints"], float)
    good = np.asarray(hand["scores"], float) >= MIN_POINT_SCORE
    if good.sum() >= 2:
        pts = pts[good]
    return float(np.ptp(pts, axis=0).max())


def agreement(ha: dict, ca: CameraGeom, hb: dict, cb: CameraGeom) -> float | None:
    """How far apart two cameras' views of a hand are: the median, over the
    points both scored, of how far each point triangulated from the two
    reprojects from what each camera saw (pixels); None with too few."""
    pa, pb = projection_matrix(ca), projection_matrix(cb)
    errors = []
    for k in range(21):
        if ha["scores"][k] < MIN_POINT_SCORE or hb["scores"][k] < MIN_POINT_SCORE:
            continue
        X = triangulate_point_dlt(
            [normalize_point(ha["keypoints"][k], ca), normalize_point(hb["keypoints"][k], cb)],
            [pa, pb],
        )
        if X is None:
            continue
        errors.append(np.linalg.norm(project_point_px(X, ca) - ha["keypoints"][k]))
        errors.append(np.linalg.norm(project_point_px(X, cb) - hb["keypoints"][k]))
    return float(np.median(errors)) if len(errors) >= 2 * MIN_SHARED_POINTS else None


def limit_px(*hands: dict) -> float:
    """The agreement two views must reach: :data:`AGREEMENT_SHARE` of the
    smaller hand, at least :data:`MIN_AGREEMENT_PX`."""
    return max(MIN_AGREEMENT_PX, AGREEMENT_SHARE * min(hand_size_px(h) for h in hands))


def sight_line(uv, cam: CameraGeom) -> tuple:
    """The camera's line of sight through pixel ``uv``: its centre and unit
    direction, in the room (mm)."""
    xn, yn = normalize_point(uv, cam)
    r, t = cam.extrinsic_rt[:3, :3], cam.extrinsic_rt[:3, 3]
    d = r @ np.array([xn, yn, 1.0])
    return t, d / np.linalg.norm(d)


def sight_gap(uv_a, ca: CameraGeom, uv_b, cb: CameraGeom) -> np.ndarray | None:
    """Where two cameras' lines of sight through what each saw of one point
    miss each other: the vector from the nearest point on ``ca``'s line to
    the nearest on ``cb``'s (mm); None for parallel lines. Its length is the
    two views' disagreement in the room, whatever each camera's zoom."""
    (pa, da), (pb, db) = sight_line(uv_a, ca), sight_line(uv_b, cb)
    w = pa - pb
    b = float(da @ db)
    denom = 1.0 - b * b
    if denom < 1e-9:
        return None
    s = (b * (db @ w) - (da @ w)) / denom
    u = ((db @ w) - b * (da @ w)) / denom
    return (pb + u * db) - (pa + s * da)


def match_tick(per_camera: dict, cameras: dict) -> tuple:
    """The hands of one tick in 3D: ``per_camera`` is ``{camera: [hands]}``.
    Per side, the two views (of two cameras) that agree best are one hand,
    then the next best of the views left, while they agree within
    :func:`limit_px`; a third camera's view of the same side, the one that
    agrees best, joins when within it too. Each hand's points are then
    triangulated from its views, a view that disagrees on a point left out
    of it.

    Returns ``(hands, compared, matched)``: each hand ``{"side", "views":
    {camera: hand}, "points": 21 x 3 (NaN where not found), "gaps": {(camera
    a, camera b): median sight gap vector (mm)}}``; how many pairs of views
    of one side were compared (different people's hands included, so not a
    rate) and how many were matched."""
    out, compared, matched = [], 0, 0
    for side in ("left", "right"):
        views = {
            c: [h for h in hs if h["side"] == side] for c, hs in per_camera.items() if c in cameras
        }
        pairs = []
        for ca, cb in combinations(sorted(views), 2):
            for ia, ha in enumerate(views[ca]):
                for ib, hb in enumerate(views[cb]):
                    e = agreement(ha, cameras[ca], hb, cameras[cb])
                    if e is not None:
                        compared += 1
                        pairs.append((e, (ca, ia), (cb, ib)))
        used: set = set()
        for e, va, vb in sorted(pairs, key=lambda p: p[0]):
            if va in used or vb in used:
                continue
            ha, hb = views[va[0]][va[1]], views[vb[0]][vb[1]]
            if e > limit_px(ha, hb):
                continue
            matched += 1
            used |= {va, vb}
            members = {va[0]: ha, vb[0]: hb}
            for c, hs in views.items():  # the third camera's best view of it
                if c in members:
                    continue
                best = None
                for i, h in enumerate(hs):
                    if (c, i) in used:
                        continue
                    e3 = agreement(ha, cameras[va[0]], h, cameras[c])
                    if e3 is not None and e3 <= limit_px(ha, h) and (best is None or e3 < best[0]):
                        best = (e3, i, h)
                if best is not None:
                    members[c] = best[2]
                    used.add((c, best[1]))
            out.append(_triangulate(side, members, cameras))
    return out, compared, matched


def _triangulate(side: str, members: dict, cameras: dict) -> dict:
    limit = limit_px(*members.values())
    points = np.full((21, 3), np.nan)
    for k in range(21):
        obs = {
            c: (h["keypoints"][k], h["scores"][k])
            for c, h in members.items()
            if h["scores"][k] >= MIN_POINT_SCORE
        }
        r = triangulate_with_rejection(obs, cameras, max_reprojection_error_px=limit)
        if r is not None:
            points[k] = r.point_room
    gaps = {}
    for ca, cb in combinations(sorted(members), 2):
        ha, hb = members[ca], members[cb]
        v = [
            sight_gap(ha["keypoints"][k], cameras[ca], hb["keypoints"][k], cameras[cb])
            for k in range(21)
            if ha["scores"][k] >= MIN_POINT_SCORE and hb["scores"][k] >= MIN_POINT_SCORE
        ]
        v = [g for g in v if g is not None]
        if v:
            gaps[(ca, cb)] = np.median(np.array(v), axis=0)
    return {"side": side, "views": members, "points": points, "gaps": gaps}


def track(ticks: list) -> list:
    """Gives each 3D hand a ``hand`` number that stays with it over time:
    ``ticks`` is ``[(time_ms, [hands])]`` in time order. A hand joins the
    nearest earlier hand of its side whose wrist (or, without one, its
    points' centre) was within :data:`TRACK_JUMP_MM` plus
    :data:`TRACK_SPEED_MM_S` times the time since, when last seen at most
    :data:`TRACK_GAP_S` ago (nearest pairs first, each once); else it starts
    a new one."""
    last: dict = {}  # hand number -> (side, time_ms, wrist)
    next_id = 1
    for t, hands in ticks:
        pairs = []
        for i, h in enumerate(hands):
            wrist = h["points"][0]
            if not np.isfinite(wrist).all():
                seen = np.isfinite(h["points"]).all(axis=1)
                wrist = h["points"][seen].mean(axis=0) if seen.any() else None
            h["_wrist"] = wrist
            if wrist is None:
                continue
            for n, (side, t0, w0) in last.items():
                if side != h["side"] or t - t0 > 1000.0 * TRACK_GAP_S:
                    continue
                d = float(np.linalg.norm(wrist - w0))
                if d <= TRACK_JUMP_MM + TRACK_SPEED_MM_S * (t - t0) / 1000.0:
                    pairs.append((d, i, n))
        taken_h, taken_n = set(), set()
        for _d, i, n in sorted(pairs):
            if i in taken_h or n in taken_n:
                continue
            hands[i]["hand"] = n
            taken_h.add(i)
            taken_n.add(n)
        for h in hands:
            if "hand" not in h:
                h["hand"] = next_id
                next_id += 1
            if h["_wrist"] is not None:
                last[h["hand"]] = (h["side"], t, h["_wrist"])
            del h["_wrist"]
    return ticks


def pair_report(ticks: list, compared: int, matched: int) -> dict:
    """Per pair of cameras, how well they agreed on the hands, in the room:
    how far their lines of sight through the same hand points miss each
    other (the median over hands of each hand's median gap, mm); the steady
    part of it (the length of the median gap vector: both cameras' room
    calibration together, as a pair cannot say which of the two is off)
    and the scatter around it (the hand model); how far apart in time the
    two cameras' frames were (median, ms: a moving hand adds to the gap);
    and the steady part from the hands whose frames were at most
    :data:`SAME_MOMENT_MS` apart, when there are enough of them.

    ``ticks`` is ``[(time_ms, [hands])]``, each hand with its ``gaps`` and,
    per pair, ``frame_gap_ms``."""
    by_pair: dict = {}
    for _t, hands in ticks:
        for h in hands:
            for pair, g in h["gaps"].items():
                e = by_pair.setdefault(pair, {"gaps": [], "dt": []})
                e["gaps"].append(g)
                e["dt"].append(h.get("frame_gap_ms", {}).get(pair, np.nan))
    pairs = []
    for (ca, cb), e in sorted(by_pair.items()):
        g, dt = np.array(e["gaps"]), np.array(e["dt"], float)
        steady = np.median(g, axis=0)
        same = np.isfinite(dt) & (dt <= SAME_MOMENT_MS)
        pairs.append(
            {
                "cameras": [ca, cb],
                "hands": len(g),
                "gap_median_mm": round(float(np.median(np.linalg.norm(g, axis=1))), 1),
                "offset_mm": round(float(np.linalg.norm(steady)), 1),
                "scatter_median_mm": round(float(np.median(np.linalg.norm(g - steady, axis=1))), 1),
                "frame_gap_median_ms": (
                    round(float(np.nanmedian(dt)), 1) if np.isfinite(dt).any() else None
                ),
                "offset_same_moment_mm": (
                    round(float(np.linalg.norm(np.median(g[same], axis=0))), 1)
                    if same.sum() >= MIN_SAME_MOMENT_HANDS
                    else None
                ),
            }
        )
    return {"views_compared": compared, "views_matched": matched, "camera_pairs": pairs}
