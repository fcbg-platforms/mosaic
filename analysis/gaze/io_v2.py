"""
Reading the room's targets and writing the gaze results.

Outputs (schema ``mosaic-gaze-fusion-v2``):

* ``<session>/gaze_fusion.json``: everything, per analysed tick and subject.
  The MOSAIC Analysis tab reads this file.
* ``<session>/gaze_fusion/gaze_fusion.csv``: one row per tick and subject.
* ``<session>/gaze_fusion/summary.json``: how long each subject looked at
  each target, and mutual gaze per pair.

Targets come from ``session_meta.json`` (the calibrated plane and the named
regions, snapshotted from the room settings when the session was recorded),
overridden by an optional ``<session>/gaze_targets.json``::

    {
      "subjects": {"S1": "Child", "S2": "Parent"},
      "regions": [
        {"name": "screen", "centre": [0, -200, 1800], "normal": [0, 0, -1],
         "u_axis": [1, 0, 0], "width": 600, "height": 340}
      ]
    }

``regions`` there replaces the session's regions; ``subjects`` renames
subjects in every output and video.
"""

from __future__ import annotations

import csv
import json
import time
from pathlib import Path

import numpy as np

from .targets import Region

SCHEMA = "mosaic-gaze-fusion-v2"


def _vec(v):
    return [round(float(x), 2) for x in v] if v is not None else None


def region_from_json(o: dict) -> Region | None:
    """A :class:`~gaze.targets.Region` from its JSON form (``None`` if invalid)."""
    try:
        return Region(
            name=str(o["name"]),
            centre=np.asarray(o["centre"], dtype=np.float64),
            normal=np.asarray(o["normal"], dtype=np.float64),
            u_axis=np.asarray(o["u_axis"], dtype=np.float64),
            width=float(o["width"]),
            height=float(o["height"]),
        )
    except (KeyError, TypeError, ValueError):
        return None


def region_to_json(r: Region) -> dict:
    return {
        "name": r.name,
        "centre": _vec(r.centre),
        "normal": [round(float(x), 5) for x in r.normal],
        "u_axis": [round(float(x), 5) for x in r.u_axis],
        "width": round(float(r.width), 1),
        "height": round(float(r.height), 1),
    }


def load_targets(session: Path, meta: dict) -> tuple[tuple | None, list[Region], dict]:
    """The session's plane, regions and subject display names.

    Returns
    -------
    tuple
        ``(plane, regions, names)``: plane as ``(point, normal)`` or None,
        a list of regions, and ``{subject_id: display_name}``.
    """
    room = meta.get("room", {}) or {}
    plane = None
    if room.get("plane_defined"):
        point, normal = room.get("plane_point"), room.get("plane_normal")
        if point and normal and np.linalg.norm(normal) > 1e-9:
            plane = (np.asarray(point, dtype=np.float64), np.asarray(normal, dtype=np.float64))
    regions = [r for r in (region_from_json(o) for o in room.get("regions", [])) if r]
    names: dict = {}

    override = session / "gaze_targets.json"
    if override.exists():
        try:
            data = json.loads(override.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            print(f"[run_gaze_fusion] Ignoring {override.name}: {e}", flush=True)
            data = {}
        if "regions" in data:
            regions = [r for r in (region_from_json(o) for o in data["regions"]) if r]
        names = {str(k): str(v) for k, v in (data.get("subjects") or {}).items()}
    return plane, regions, names


def display(subject: str | None, names: dict) -> str:
    """A subject's display name (its id when not renamed)."""
    if subject is None:
        return ""
    return names.get(subject, subject)


def entry_json(e, names: dict) -> dict:
    t = e.target
    return {
        "id": e.subject,
        "name": display(e.subject, names),
        "origin": _vec(e.origin),
        "direction": [round(float(x), 5) for x in e.direction] if e.direction is not None else None,
        "point": _vec(t.point) if t is not None else None,
        "target": (
            {
                "type": e.label_kind,
                "label": display(e.label, names) if e.label_kind == "subject" else e.label,
                "subject": e.label if e.label_kind == "subject" else None,
                "distance_mm": round(float(t.distance), 1),
                "measured": t.label if t.label != e.label else None,
            }
            if t is not None and e.label is not None
            else None
        ),
        "mutual": bool(e.mutual),
        "confidence": round(float(e.confidence), 3),
        "uncertainty_deg": round(float(e.uncertainty_deg), 2)
        if e.uncertainty_deg is not None
        else None,
        "n_cameras": e.n_cameras,
        "face_scale": round(float(e.pose.scale), 3),
        "per_camera": [
            {
                "camera": c.camera,
                "face_box_px": [round(float(v), 1) for v in c.box],
                "direction": [round(float(x), 5) for x in c.direction]
                if c.direction is not None
                else None,
                "weight": round(float(c.weight), 3),
            }
            for c in e.per_camera
        ],
    }


def write_results(
    session: Path,
    timeline,
    entries: list,
    analysed_ticks: list[int],
    subjects: list[str],
    names: dict,
    cameras: dict,
    plane,
    regions: list[Region],
    skip: int,
    videos: dict,
    room_video: str | None,
) -> Path:
    """Write ``gaze_fusion.json``; returns its path."""
    by_tick: dict = {}
    for e in entries:
        by_tick.setdefault(e.tick, []).append(e)
    scales = {}
    for s in subjects:
        mine = [e.pose.scale for e in entries if e.subject == s and e.single_cam is None]
        scales[s] = round(float(np.median(mine)), 3) if mine else None
    counts = {s: sum(1 for e in entries if e.subject == s) for s in subjects}

    data = {
        "schema": SCHEMA,
        "generated_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source": timeline.source,
        "fps": timeline.fps,
        "n_ticks": timeline.n_ticks,
        "analysed_every": skip,
        "first_tick": timeline.first_tick,
        "source_videos": {
            str(i): _rel(session, t.video_path) for i, t in sorted(timeline.cameras.items())
        },
        "annotated_videos": {str(k): v for k, v in sorted(videos.items())},
        "room_video": room_video,
        "cameras": [
            {
                "index": i,
                "position_room": _vec(c.extrinsic_rt[:3, 3]),
                "room_from_camera": [round(float(x), 6) for x in c.extrinsic_rt.ravel()],
            }
            for i, c in sorted(cameras.items())
        ],
        "plane": {
            "defined": plane is not None,
            "point": _vec(plane[0]) if plane else None,
            "normal": [round(float(x), 5) for x in plane[1]] if plane else None,
        },
        "regions": [region_to_json(r) for r in regions],
        "subjects": [
            {"id": s, "name": display(s, names), "frames": counts[s], "face_scale": scales[s]}
            for s in subjects
        ],
        "frames": [
            {
                "tick": k,
                "timestamp_ns": int(timeline.times_ns[k]),
                "video_frame_index": k,
                "subjects": [
                    entry_json(e, names)
                    for e in sorted(by_tick.get(k, []), key=lambda e: e.subject)
                ],
            }
            for k in analysed_ticks
        ],
    }
    out = session / "gaze_fusion.json"
    tmp = out.with_suffix(".json.part")
    tmp.write_text(json.dumps(data, indent=1), encoding="utf-8")
    tmp.replace(out)
    return out


def write_csv(path: Path, timeline, entries: list, names: dict) -> None:
    """One row per analysed tick and subject."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "tick",
                "timestamp_ns",
                "video_frame_index",
                "subject",
                "origin_x_mm",
                "origin_y_mm",
                "origin_z_mm",
                "dir_x",
                "dir_y",
                "dir_z",
                "point_x_mm",
                "point_y_mm",
                "point_z_mm",
                "target_type",
                "target",
                "mutual",
                "confidence",
                "uncertainty_deg",
                "n_cameras",
            ]
        )
        for e in sorted(entries, key=lambda e: (e.tick, e.subject)):
            d = e.direction if e.direction is not None else [None] * 3
            p = e.target.point if e.target is not None else [None] * 3
            label = e.label or ""
            w.writerow(
                [
                    e.tick,
                    int(timeline.times_ns[e.tick]),
                    e.tick,
                    display(e.subject, names),
                    *(_fmt(v, 1) for v in e.origin),
                    *(_fmt(v, 5) for v in d),
                    *(_fmt(v, 1) for v in p),
                    e.label_kind or "",
                    display(label, names) if e.label_kind == "subject" else label,
                    "true" if e.mutual else "false",
                    _fmt(e.confidence, 3),
                    _fmt(e.uncertainty_deg, 2),
                    e.n_cameras,
                ]
            )


def summarise(
    entries: list, subjects: list[str], names: dict, tick_s: float, n_ticks: int, skip: int
) -> dict:
    """Dwell time per subject and target, and mutual gaze per pair.

    Each analysed tick stands for ``skip`` ticks of ``tick_s`` seconds.
    """
    per = skip * tick_s
    out: dict = {"seconds_per_analysed_tick": round(per, 4), "subjects": {}, "mutual_gaze": {}}
    analysed_ticks = max(1, (n_ticks + skip - 1) // skip)
    for s in subjects:
        mine = [e for e in entries if e.subject == s]
        with_gaze = [e for e in mine if e.direction is not None and e.label]
        dwell: dict = {}
        for e in with_gaze:
            key = display(e.label, names) if e.label_kind == "subject" else e.label
            dwell[key] = dwell.get(key, 0.0) + per
        out["subjects"][display(s, names)] = {
            "id": s,
            "seen_s": round(len(mine) * per, 2),
            "gaze_s": round(len(with_gaze) * per, 2),
            "coverage_pct": round(100.0 * len(mine) / analysed_ticks, 1),
            "looked_at_s": {
                k: round(v, 2) for k, v in sorted(dwell.items(), key=lambda kv: -kv[1])
            },
        }
    pairs: dict = {}
    for e in entries:
        if e.mutual:
            pair = tuple(sorted((e.subject, e.label)))
            pairs.setdefault(pair, set()).add(e.tick)
    for (a, b), ticks in sorted(pairs.items()):
        out["mutual_gaze"][f"{display(a, names)} & {display(b, names)}"] = {
            "seconds": round(len(ticks) * per, 2),
            "pct_of_recording": round(100.0 * len(ticks) / analysed_ticks, 1),
        }
    return out


def _fmt(v, digits: int):
    return "" if v is None else f"{float(v):.{digits}f}"


def _rel(session: Path, p: Path) -> str:
    try:
        return p.relative_to(session).as_posix()
    except ValueError:
        return str(p)
