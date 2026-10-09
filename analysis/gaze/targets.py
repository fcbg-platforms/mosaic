"""
What a gaze ray lands on: another subject's face, a named region, the
calibrated plane, or nothing (a free point along the ray).

Every candidate the ray meets is collected and the nearest one along the ray
wins, as in the real world: looking at a person standing behind a screen
lands on the screen.

**Faces** are not hit-tested as surfaces. Gaze estimation is accurate to a
few degrees, while a head subtends only about 5 degrees at 2.5 m; testing
the ray against a 110 mm sphere would miss most real looks at a face. A
face counts as looked at when the angle between the gaze and the direction
to that face is below the larger of the face's angular radius and
:data:`FACE_MIN_CONE_DEG`.

Labels then go through :func:`apply_min_dwell`, so a single noisy tick
cannot flip "S1 looks at S2" to something else and back.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

from .ray_math import angle_deg, ray_plane_intersection, unit

#: Radius of a head, mm (half a typical head breadth plus margin).
FACE_RADIUS_MM = 110.0
#: Smallest angular tolerance for "looking at a face", degrees: about the
#: angular accuracy of calibration-free gaze estimation.
FACE_MIN_CONE_DEG = 7.0
#: Where the gaze point is drawn when the ray hits nothing, mm along the ray.
FREE_POINT_DISTANCE_MM = 1500.0


@dataclass
class Region:
    """A named rectangle in room coordinates.

    Attributes
    ----------
    name : str
        Label shown in videos and outputs, e.g. ``"screen"``.
    centre : numpy.ndarray
        Rectangle centre, mm.
    normal : numpy.ndarray
        Unit normal.
    u_axis : numpy.ndarray
        Unit vector along the rectangle's width (perpendicular to
        ``normal``); the height runs along ``normal x u_axis``.
    width, height : float
        Size in mm.
    """

    name: str
    centre: np.ndarray
    normal: np.ndarray
    u_axis: np.ndarray
    width: float
    height: float

    def __post_init__(self) -> None:
        self.centre = np.asarray(self.centre, dtype=np.float64)
        self.normal = unit(self.normal)
        u = np.asarray(self.u_axis, dtype=np.float64)
        # Keep u exactly perpendicular to the normal.
        self.u_axis = unit(u - np.dot(u, self.normal) * self.normal)

    @property
    def v_axis(self) -> np.ndarray:
        return np.cross(self.normal, self.u_axis)

    def corners(self) -> np.ndarray:
        """The four corners, ``(4, 3)``, in order around the rectangle."""
        hu, hv = self.u_axis * self.width / 2.0, self.v_axis * self.height / 2.0
        c = self.centre
        return np.array([c - hu - hv, c + hu - hv, c + hu + hv, c - hu + hv])


@dataclass
class TargetHit:
    """Where one subject's gaze lands at one tick.

    Attributes
    ----------
    kind : str
        ``"subject"``, ``"region"``, ``"plane"`` or ``"none"``.
    label : str
        Display label: a subject name, a region name, ``"plane"`` or
        ``"none"``.
    point : numpy.ndarray
        The 3D gaze point, mm.
    distance : float
        Distance from the gaze origin to ``point``, mm.
    subject : str or None
        The looked-at subject's id when ``kind == "subject"``.
    angle_off_deg : float or None
        For faces: angle between the gaze and the direction to the face.
    """

    kind: str
    label: str
    point: np.ndarray
    distance: float
    subject: str | None = None
    angle_off_deg: float | None = None
    extra: dict = field(default_factory=dict)


def _hit_region(origin, direction, region: Region):
    hit = ray_plane_intersection(origin, direction, region.centre, region.normal)
    if hit is None:
        return None
    point, t = hit
    rel = point - region.centre
    if (
        abs(np.dot(rel, region.u_axis)) <= region.width / 2.0
        and abs(np.dot(rel, region.v_axis)) <= region.height / 2.0
    ):
        return point, t
    return None


def cast_gaze(
    origin,
    direction,
    faces: dict | None = None,
    regions: list[Region] | None = None,
    plane: tuple | None = None,
    face_radius_mm: float = FACE_RADIUS_MM,
    min_cone_deg: float = FACE_MIN_CONE_DEG,
    free_distance_mm: float = FREE_POINT_DISTANCE_MM,
) -> TargetHit:
    """The nearest target along a gaze ray.

    Parameters
    ----------
    origin, direction : array_like
        Gaze ray (origin between the eyes, mm).
    faces : dict, optional
        ``{subject_id: eye_midpoint}`` of the *other* subjects at this tick.
    regions : list of Region, optional
        Named rectangles.
    plane : tuple, optional
        ``(point, normal)`` of the calibrated plane.
    face_radius_mm, min_cone_deg : float
        Face test, see the module docstring.
    free_distance_mm : float
        Distance of the free point when nothing is hit.

    Returns
    -------
    TargetHit
    """
    o = np.asarray(origin, dtype=np.float64)
    d = unit(direction)
    candidates: list[TargetHit] = []

    for sid, head in (faces or {}).items():
        to_face = np.asarray(head, dtype=np.float64) - o
        dist = float(np.linalg.norm(to_face))
        if dist < 1e-6 or np.dot(to_face, d) <= 0.0:
            continue
        off = angle_deg(d, to_face)
        cone = max(math.degrees(math.asin(min(1.0, face_radius_mm / dist))), min_cone_deg)
        if off <= cone:
            # The gaze point is where the ray passes the face, at its
            # distance, not the face centre: the residual angle stays visible.
            candidates.append(
                TargetHit(
                    "subject", str(sid), o + d * dist, dist, subject=str(sid), angle_off_deg=off
                )
            )

    for region in regions or []:
        hit = _hit_region(o, d, region)
        if hit is not None:
            candidates.append(TargetHit("region", region.name, hit[0], hit[1]))

    if plane is not None:
        hit = ray_plane_intersection(o, d, plane[0], plane[1])
        if hit is not None:
            candidates.append(TargetHit("plane", "plane", hit[0], hit[1]))

    if candidates:
        # Faces are judged by angle, not by a surface hit, so a face just
        # in front of a region is not lost to it by a few millimetres.
        return min(candidates, key=lambda c: c.distance)
    return TargetHit("none", "none", o + d * free_distance_mm, free_distance_mm)


def apply_min_dwell(labels, times_s, min_dwell_s: float = 0.15, max_gap_s: float = 0.5) -> list:
    """Suppress label changes that do not last.

    Parameters
    ----------
    labels : sequence
        One label per sample (``None`` where there is no estimate).
    times_s : array_like
        Sample times in seconds, increasing.
    min_dwell_s : float
        A run of one label shorter than this is replaced by the label before
        it (or, at the start of a stretch, the one after it).
    max_gap_s : float
        A time gap longer than this ends a stretch: labels never carry
        across it, in either direction.

    Returns
    -------
    list
        The debounced labels. ``None`` stays ``None`` and is not a change.

    Notes
    -----
    A run's length is its number of samples times the typical sample
    spacing, so a run next to a gap is not credited with the gap's length.
    """
    out = list(labels)
    t = np.asarray(times_s, dtype=np.float64)
    known = [i for i, v in enumerate(out) if v is not None]
    if len(known) < 2:
        return out
    step = float(np.median(np.diff(t[known])))
    # Stretches of known samples without long time gaps.
    stretches, current = [], [known[0]]
    for a, b in zip(known, known[1:], strict=False):
        if t[b] - t[a] > max_gap_s:
            stretches.append(current)
            current = []
        current.append(b)
    stretches.append(current)

    for idx in stretches:
        # Runs of equal labels within the stretch: (label, sample indices).
        runs: list[tuple[object, list[int]]] = []
        for i in idx:
            if runs and runs[-1][0] == out[i]:
                runs[-1][1].append(i)
            else:
                runs.append((out[i], [i]))
        if len(runs) < 2:
            continue
        kept: list[tuple[object, list[int]]] = []
        for label, members in runs:
            if len(members) * step >= min_dwell_s or not kept:
                if kept and kept[-1][0] == label:
                    kept[-1][1].extend(members)
                else:
                    kept.append((label, list(members)))
            else:
                kept[-1][1].extend(members)  # too short: keep the previous label
        # A short run at the very start of the stretch has no label before
        # it: give it the one after it.
        if len(kept) > 1 and len(kept[0][1]) * step < min_dwell_s:
            kept[1][1][:0] = kept[0][1]
            kept.pop(0)
        for label, members in kept:
            for i in members:
                out[i] = label
    return out
