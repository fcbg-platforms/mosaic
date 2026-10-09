"""
Subjects across cameras and time, and one fused gaze per subject per tick.

Per tick:

1. **Association.** Every face first gets its own head position from
   single-camera PnP (depth good to about 10%). Faces from different cameras
   are the same person when those positions agree, and when their midline
   landmarks also triangulate consistently. Position comes first because
   triangulation alone is fooled when cameras and heads sit at one height:
   two rays in one plane always meet, so any two faces would match. Faces
   no other camera matched stay as single-camera subjects.
2. **Head pose.** One rigid head per subject: PnP in the camera with the
   largest view of the face, then :func:`gaze.head_pose.refine_joint` over
   every camera seeing it, which also measures the face scale when two or
   more cameras see it.
3. **Eyes.** Every camera that sees an eye contributes a visual axis
   (:mod:`gaze.eye_model`); all of them are combined by a weighted mean that
   drops directions far from the rest (:func:`gaze.ray_math.robust_mean_direction`).
   The origin is the midpoint of the two eyeball centres, from the head pose.

Over the whole recording (:func:`assign_subjects`): head positions are
tracked from tick to tick, broken tracks of the same seated person are
joined, and subjects are named ``S1``, ``S2``... from left to right. A
subject seen by one camera at a tick gets their face scale from the ticks
where several cameras saw them (:func:`apply_subject_scale`).
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

from .canonical import (
    LEFT_IRIS,
    MIDLINE_IDS,
    RIGHT_IRIS,
    CanonicalFace,
)
from .eye_model import EYE_CENTRE_DEPTH_MM, EYES, estimate_eye, eye_openness
from .head_pose import (
    CameraView,
    HeadPose,
    camera_pose_to_room,
    refine_joint,
    solve_pnp,
)
from .ray_math import robust_mean_direction, unit

#: Directions further than this from the consensus are dropped.
REJECT_DEG = 20.0
#: Single-camera head depth is good to about this share of the distance
#: (real faces differ from the canonical one by up to ~8%, plus fit noise).
DEPTH_TOLERANCE = 0.15
#: A joint head fit across cameras worse than this (px RMS) explains the
#: landmarks too poorly to be one person: the faces are kept apart.
MAX_JOINT_RMS_PX = 8.0


@dataclass
class FaceObs:
    """One face in one camera at one tick.

    Attributes
    ----------
    camera : int
        Camera index.
    box : numpy.ndarray
        Face box ``(x1, y1, x2, y2)``, px.
    points : numpy.ndarray
        Landmark pixels, ordered like the rig's ``ids``.
    score : float
        Detector confidence.
    """

    camera: int
    box: np.ndarray
    points: np.ndarray
    score: float = 1.0
    # This face's own single-camera PnP (camera from head), filled on first
    # use by GazeRig.view_pose(); False means it failed.
    pnp: object = None

    @property
    def size(self) -> float:
        return float(max(self.box[2] - self.box[0], self.box[3] - self.box[1]))


@dataclass
class CameraGaze:
    """One camera's contribution to a subject's gaze at one tick."""

    camera: int
    box: np.ndarray
    direction: np.ndarray | None
    weight: float


@dataclass
class Entry:
    """One subject at one tick: head, gaze, and how it was obtained."""

    tick: int
    views: list
    pose: HeadPose  # room from head
    single_cam: tuple | None = None  # (camera, camera-from-head pose) when one view
    track: int = -1
    subject: str | None = None
    origin: np.ndarray | None = None
    eye_centres: tuple | None = None
    direction: np.ndarray | None = None
    confidence: float = 0.0
    uncertainty_deg: float | None = None
    dispersion_deg: float | None = None
    per_camera: list = field(default_factory=list)
    # Filled after smoothing (run_gaze_fusion.finalize): the unsmoothed
    # direction, what the gaze landed on, the debounced label, mutual gaze.
    raw_direction: np.ndarray | None = None
    target: object | None = None
    label: str | None = None  # debounced target label
    label_kind: str | None = None  # its type: subject, region, plane, none
    mutual: bool = False

    @property
    def n_cameras(self) -> int:
        return len(self.views)


class GazeRig:
    """Calibrated cameras plus the canonical face: everything per-tick fusion
    needs.

    Parameters
    ----------
    cameras : dict of int to pose3d.triangulation.CameraGeom
    canonical : CanonicalFace
    ids : array_like
        Landmark ids, in the order of every :class:`FaceObs` ``points``.
    """

    def __init__(self, cameras: dict, canonical: CanonicalFace, ids) -> None:
        self.cameras = cameras
        self.canonical = canonical
        self.ids = np.asarray(ids, dtype=np.int64)
        self._pos = {int(i): k for k, i in enumerate(self.ids)}
        self._rigid = [i for i in canonical.rigid_ids if i in self._pos]
        self._rigid_rows = np.array([self._pos[i] for i in self._rigid])
        self._rigid_model = canonical.points(self._rigid)
        weights = dict(zip(canonical.rigid_ids, canonical.rigid_weights, strict=False))
        self._rigid_weights = np.array([weights[i] for i in self._rigid])
        self._midline_rows = [self._pos[i] for i in MIDLINE_IDS if i in self._pos]

    def row(self, landmark_id: int) -> int:
        return self._pos[landmark_id]

    # ── 1. association ──────────────────────────────────────────────────────

    def view_pose(self, face: FaceObs) -> HeadPose | None:
        """A face's own single-camera PnP (camera frame, canonical scale),
        computed once and cached on the face."""
        if face.pnp is None:
            cam = self.cameras[face.camera]
            pnp = solve_pnp(
                self._rigid_model, face.points[self._rigid_rows], cam.camera_matrix, cam.dist_coeffs
            )
            face.pnp = pnp if pnp is not None else False
        return face.pnp or None

    def _same_person(self, a: FaceObs, b: FaceObs, max_pair_cost_px: float) -> float | None:
        """How well two faces from different cameras fit one person: the gap
        (mm) between their sight lines, or ``None`` when they cannot be one.

        Each camera's head estimate is uncertain mostly along its own line of
        sight (depth). So the test is not the distance between the two
        estimates: the two sight lines (camera centre through the head) must
        nearly meet, and where they meet must lie within each estimate's depth
        band. With cameras and heads at one height every pair of sight lines
        meets somewhere; for two different people that point lies far
        outside the bands. The midline landmarks must also triangulate
        consistently.
        """
        from pose3d.association import PersonObservation, pairwise_cost

        pa, pb = self.view_pose(a), self.view_pose(b)
        if pa is None or pb is None:
            return None
        ca, cb = self.cameras[a.camera], self.cameras[b.camera]
        oa, ob = ca.extrinsic_rt[:3, 3], cb.extrinsic_rt[:3, 3]
        ha = camera_pose_to_room(pa, ca.extrinsic_rt).t
        hb = camera_pose_to_room(pb, cb.extrinsic_rt).t
        da, db = np.linalg.norm(ha - oa), np.linalg.norm(hb - ob)
        ua, ub = (ha - oa) / da, (hb - ob) / db
        # Closest points of the two sight lines, at distances sa and sb.
        w = oa - ob
        bb = float(np.dot(ua, ub))
        denom = 1.0 - bb * bb
        if denom < 1e-6:
            return None  # parallel sight lines: no depth information
        sa = (bb * np.dot(ub, w) - np.dot(ua, w)) / denom
        sb = (np.dot(ub, w) - bb * np.dot(ua, w)) / denom
        gap = float(np.linalg.norm((oa + sa * ua) - (ob + sb * ub)))
        if gap > 150.0:
            return None
        if abs(sa - da) > DEPTH_TOLERANCE * da or abs(sb - db) > DEPTH_TOLERANCE * db:
            return None
        obs = [
            PersonObservation(
                f.camera, 0, f.points[self._midline_rows], np.ones(len(self._midline_rows))
            )
            for f in (a, b)
        ]
        if pairwise_cost(obs[0], obs[1], ca, cb) > max_pair_cost_px:
            return None
        return gap

    def associate(
        self, faces_by_camera: dict, max_pair_cost_px: float = 25.0
    ) -> list[list[FaceObs]]:
        """Group one tick's faces into subjects (one list per subject).

        Pairs that pass :meth:`_same_person` are joined best first. Two groups
        join only when every face of one fits every face of the other, and
        no camera contributes two faces: a chain A~B, B~C never joins A to C
        on its own.

        Parameters
        ----------
        faces_by_camera : dict
            ``{camera: [FaceObs, ...]}``.
        max_pair_cost_px : float
            Mean two-view reprojection error allowed for the midline landmarks.
        """
        faces = [
            f for cam, fs in sorted(faces_by_camera.items()) if cam in self.cameras for f in fs
        ]
        fits: dict = {}
        for i in range(len(faces)):
            for j in range(i + 1, len(faces)):
                if faces[i].camera != faces[j].camera:
                    gap = self._same_person(faces[i], faces[j], max_pair_cost_px)
                    if gap is not None:
                        fits[(i, j)] = gap

        group_of = list(range(len(faces)))
        members = {i: {i} for i in range(len(faces))}
        for (i, j), _gap in sorted(fits.items(), key=lambda kv: kv[1]):
            gi, gj = group_of[i], group_of[j]
            if gi == gj:
                continue
            if {faces[k].camera for k in members[gi]} & {faces[k].camera for k in members[gj]}:
                continue
            if not all((min(x, y), max(x, y)) in fits for x in members[gi] for y in members[gj]):
                continue
            for k in members[gj]:
                group_of[k] = gi
            members[gi] |= members.pop(gj)
        return [[faces[k] for k in sorted(m)] for m in members.values()]

    # ── 2. head pose ────────────────────────────────────────────────────────

    def head_pose(self, views: list[FaceObs], tick: int) -> list[Entry]:
        """Room head pose of one subject from all the cameras seeing it.

        Returns one entry, or, when the faces cannot be fitted as one head
        (joint RMS above :data:`MAX_JOINT_RMS_PX`), one entry per face, so a
        wrong association never becomes a compromise between two people.
        """
        ordered = sorted(views, key=lambda f: -f.size)
        first = ordered[0]
        cam = self.cameras[first.camera]
        pnp = self.view_pose(first)
        if pnp is None:
            return [e for f in ordered[1:] for e in self.head_pose([f], tick)]
        if len(ordered) == 1:
            room = camera_pose_to_room(pnp, cam.extrinsic_rt)
            return [Entry(tick=tick, views=ordered, pose=room, single_cam=(first.camera, pnp))]
        cam_views = [
            CameraView(
                camera_from_room=self.cameras[f.camera].camera_from_room,
                camera_matrix=self.cameras[f.camera].camera_matrix,
                dist_coeffs=self.cameras[f.camera].dist_coeffs,
                model_pts=self._rigid_model,
                image_pts=f.points[self._rigid_rows],
                weights=self._rigid_weights,
            )
            for f in ordered
        ]
        pose = refine_joint(cam_views, camera_pose_to_room(pnp, cam.extrinsic_rt), fit_scale=True)
        if pose.rms_px > MAX_JOINT_RMS_PX:
            return [e for f in ordered for e in self.head_pose([f], tick)]
        return [Entry(tick=tick, views=ordered, pose=pose)]

    # ── 3. gaze ─────────────────────────────────────────────────────────────

    def eye_centres_room(self, pose: HeadPose) -> dict:
        """Both eyeball centres in the room for a room-from-head pose."""
        out = {}
        for name, (outer, inner, _top, _bottom, _nasal) in EYES.items():
            mid = (self.canonical.vertices_mm[outer] + self.canonical.vertices_mm[inner]) / 2.0
            centre_head = pose.scale * mid + np.array([0.0, 0.0, EYE_CENTRE_DEPTH_MM])
            out[name] = pose.r @ centre_head + pose.t
        return out

    def compute_gaze(self, entry: Entry) -> None:
        """Fill ``entry``'s origin, direction, confidence and per-camera data."""
        import cv2

        centres_room = self.eye_centres_room(entry.pose)
        entry.eye_centres = (centres_room["right"], centres_room["left"])
        entry.origin = (centres_room["right"] + centres_room["left"]) / 2.0

        dirs, weights, sigmas, owners, eyes = [], [], [], [], []
        for face in entry.views:
            cam = self.cameras[face.camera]
            r_cr = cam.camera_from_room[:3, :3]
            t_cr = cam.camera_from_room[:3, 3]
            r_cam_head = r_cr @ entry.pose.r
            for name, (outer, inner, top, bottom, nasal) in EYES.items():
                iris = RIGHT_IRIS if name == "right" else LEFT_IRIS
                pts = face.points
                centre_px = pts[self._pos[iris[0]]]
                rim = pts[[self._pos[i] for i in iris[1:]]]
                iris_r_px = float(np.mean(np.linalg.norm(rim - centre_px, axis=1)))
                openness = eye_openness(
                    pts[self._pos[outer]],
                    pts[self._pos[inner]],
                    pts[self._pos[top]],
                    pts[self._pos[bottom]],
                )
                norm = cv2.undistortPoints(
                    centre_px.reshape(1, 1, 2).astype(np.float64),
                    cam.camera_matrix,
                    cam.dist_coeffs,
                ).reshape(2)
                ray = np.array([norm[0], norm[1], 1.0])
                centre_cam = r_cr @ centres_room[name] + t_cr
                est = estimate_eye(r_cam_head, centre_cam, ray, nasal, openness, iris_r_px)
                if est.weight <= 0.0:
                    continue
                dirs.append(r_cr.T @ est.visual_axis_cam)  # camera -> room
                weights.append(est.weight)
                sigmas.append(est.sigma_deg)
                owners.append(face.camera)
                eyes.append(name)

        entry.per_camera = []
        combined = _combine_eyes(dirs, weights, eyes)
        if combined is None:
            entry.direction = None
            entry.confidence = 0.0
            for face in entry.views:
                entry.per_camera.append(CameraGaze(face.camera, face.box, None, 0.0))
            return
        mean, dispersion, kept, used = combined
        entry.direction = mean
        if len(used) == 1:
            # One eye: the gaze ray starts at that eye, not between the eyes,
            # or it would run parallel to the true line of sight, 32 mm off.
            entry.origin = centres_room[used[0]]
        entry.dispersion_deg = dispersion
        w = np.asarray(weights)
        s = np.asarray(sigmas)
        # 1-sigma angular uncertainty of this tick's (unsmoothed) estimate:
        # the kept observations' resolution-limited noise, combined as
        # independent measurements, plus how much they actually disagree.
        # The reliability weights choose which observations count; they do
        # not shrink an observation's noise.
        n_kept = int(np.sum(kept))
        info = float(np.sum(1.0 / np.maximum(s[kept], 1e-3) ** 2))
        noise = 1.0 / math.sqrt(info) if info > 0 else 90.0
        entry.uncertainty_deg = math.sqrt(noise**2 + dispersion**2 / max(n_kept, 1))
        entry.confidence = float(1.0 - math.exp(-float(np.sum(w[kept]))))
        owners_arr = np.asarray(owners)
        d = np.asarray(dirs)
        for face in entry.views:
            mine = (owners_arr == face.camera) & kept
            cam_dir = unit((w[mine, None] * d[mine]).sum(axis=0)) if mine.any() else None
            entry.per_camera.append(
                CameraGaze(
                    face.camera, face.box, cam_dir, float(w[owners_arr == face.camera].sum())
                )
            )


def _combine_eyes(dirs, weights, eyes, one_eye_ratio: float = 0.25):
    """Fused gaze from every camera's view of each eye.

    Each eye is first averaged over the cameras (robustly). The two eyes
    then count equally: they converge on what the subject looks at, about
    2.5 degrees apart at 1.5 m, and weighting one more just because a camera
    sees it better would tilt the result towards it. When one eye is much
    less reliable than the other (a closed eye, or hidden behind the nose),
    the better eye alone is used.

    Returns
    -------
    tuple or None
        ``(direction, dispersion_deg, kept_mask, eyes_used)``: as
        :func:`gaze.ray_math.robust_mean_direction`, plus which eyes
        (``"right"``, ``"left"``) the direction comes from.
    """
    if not dirs:
        return None
    d = np.asarray(dirs, dtype=np.float64)
    w = np.asarray(weights, dtype=np.float64)
    which = np.asarray(eyes)
    kept = np.zeros(len(d), dtype=bool)
    per_eye = {}
    for name in ("right", "left"):
        idx = np.flatnonzero(which == name)
        if idx.size == 0:
            continue
        res = robust_mean_direction(d[idx], w[idx], reject_deg=REJECT_DEG)
        if res is None:
            continue
        per_eye[name] = (res[0], float(w[idx][res[2]].sum()))
        kept[idx[res[2]]] = True
    if not per_eye:
        return None
    if len(per_eye) == 2:
        (dr, wr), (dl, wl) = per_eye["right"], per_eye["left"]
        if min(wr, wl) >= one_eye_ratio * max(wr, wl):
            mean, used = unit(dr + dl), ["right", "left"]
        else:
            weaker = "right" if wr < wl else "left"
            stronger = "left" if weaker == "right" else "right"
            mean, used = per_eye[stronger][0], [stronger]
            kept &= which != weaker
    else:
        ((name, (mean, _w)),) = per_eye.items()
        used = [name]
    # Spread of the kept per-camera directions around their own eye's mean,
    # so the eyes' natural convergence does not count as disagreement.
    spread = np.zeros(len(d))
    for name, (eye_mean, _w) in per_eye.items():
        sel = kept & (which == name)
        spread[sel] = np.degrees(np.arccos(np.clip(d[sel] @ eye_mean, -1.0, 1.0)))
    dispersion = float(math.sqrt(np.average(spread[kept] ** 2, weights=w[kept])))
    return mean, dispersion, kept, used


# ── Over the whole recording ─────────────────────────────────────────────────


def apply_subject_scale(entries: list[Entry], cameras: dict) -> None:
    """Give single-camera entries their subject's face scale.

    The scale is the median over that subject's multi-camera entries (1.0
    when there are none). Scaling a face and its distance together leaves
    the image unchanged, so the single-camera pose just scales in the
    camera's frame.
    """
    by_subject: dict = {}
    for e in entries:
        if e.single_cam is None and e.subject is not None:
            by_subject.setdefault(e.subject, []).append(e.pose.scale)
    for e in entries:
        if e.single_cam is None:
            continue
        s = float(np.median(by_subject[e.subject])) if by_subject.get(e.subject) else 1.0
        cam, pnp = e.single_cam
        scaled = HeadPose(r=pnp.r, t=pnp.t * s, scale=s, rms_px=pnp.rms_px)
        e.pose = camera_pose_to_room(scaled, cameras[cam].extrinsic_rt)


def assign_subjects(
    entries: list[Entry],
    times_s,
    max_gap_s: float = 2.0,
    max_jump_mm: float = 400.0,
    merge_distance_mm: float = 500.0,
    walk_mm_s: float = 1000.0,
    min_seconds: float = 1.0,
    max_subjects: int | None = None,
) -> list[str]:
    """Track heads over time and name subjects ``S1``, ``S2``...

    Parameters
    ----------
    entries : list of Entry
        Every subject entry of every analysed tick, any order. Each gets
        ``track`` and ``subject`` set (``subject`` stays ``None`` for
        entries of discarded tracks).
    times_s : array_like
        Time of every tick, seconds (indexed by ``Entry.tick``).
    max_gap_s : float
        A track survives this long without being seen.
    max_jump_mm : float
        Largest head movement between two sightings of one track.
    merge_distance_mm, walk_mm_s : float
        Two tracks never seen at the same tick are one person when, at each
        hand-over between them, the head moved at most ``merge_distance_mm``
        plus ``walk_mm_s`` times the time between the two sightings: the
        same seated person who looked away, or someone who got up and came
        back.
    min_seconds : float
        Tracks shorter than this (in seen time) are discarded as noise.
    max_subjects : int, optional
        Keep only this many subjects, those seen the longest.

    Returns
    -------
    list of str
        The subject names, in order.
    """
    from pose3d.tracker import PersonTracker3D

    times = np.asarray(times_s, dtype=np.float64)
    if not entries:
        return []
    step = float(np.median(np.diff(times))) if len(times) > 1 else 1.0
    by_tick: dict = {}
    for e in entries:
        by_tick.setdefault(e.tick, []).append(e)
    tracker = PersonTracker3D(
        max_gap_ticks=max(1, int(round(max_gap_s / max(step, 1e-6)))), max_jump_mm=max_jump_mm
    )
    for tick in sorted(by_tick):
        group = by_tick[tick]
        ids = tracker.update(tick, [e.pose.t.copy() for e in group])
        for e, tid in zip(group, ids, strict=True):
            e.track = tid

    # Summaries per track: when it was seen and where it sat.
    tracks: dict = {}
    for e in entries:
        tracks.setdefault(e.track, []).append(e)
    info = {
        tid: {
            "ticks": {e.tick for e in es},
            "pos": np.median(np.array([e.pose.t for e in es]), axis=0),
        }
        for tid, es in tracks.items()
    }

    # Join tracks of one person: never seen at the same tick, and every
    # hand-over between them is walkable (see _walkable). Closest first.
    parent = {tid: tid for tid in info}

    def root(t):
        while parent[t] != t:
            t = parent[t]
        return t

    samples = {
        tid: sorted((float(times[e.tick]), e.pose.t) for e in es) for tid, es in tracks.items()
    }
    pairs = sorted(
        (float(np.linalg.norm(info[a]["pos"] - info[b]["pos"])), a, b)
        for a in info
        for b in info
        if a < b
    )
    merged_ticks = {tid: set(v["ticks"]) for tid, v in info.items()}
    merged_samples = dict(samples)
    for _dist, a, b in pairs:
        ra, rb = root(a), root(b)
        if ra == rb or merged_ticks[ra] & merged_ticks[rb]:
            continue
        if not _walkable(merged_samples[ra], merged_samples[rb], merge_distance_mm, walk_mm_s):
            continue
        parent[rb] = ra
        merged_ticks[ra] |= merged_ticks.pop(rb)
        merged_samples[ra] = sorted(merged_samples[ra] + merged_samples.pop(rb), key=lambda x: x[0])

    people: dict = {}
    for tid, es in tracks.items():
        people.setdefault(root(tid), []).extend(es)

    # Seen time in analysed time: each analysed tick stands for the spacing
    # between analysed ticks (2 ticks with --skip 2), not for one tick.
    analysed = sorted(by_tick)
    per_sample = float(np.median(np.diff(times[analysed]))) if len(analysed) > 1 else step
    people = _join_split_people(people)
    seen_s = {r: len({e.tick for e in es}) * per_sample for r, es in people.items()}
    keep = [r for r in people if seen_s[r] >= min_seconds]
    keep.sort(key=lambda r: -seen_s[r])
    if max_subjects is not None:
        keep = keep[:max_subjects]
    # Left to right in the room (reference camera x), so names are stable
    # between runs of the same session.
    keep.sort(key=lambda r: float(np.median([e.pose.t[0] for e in people[r]])))
    names = {r: f"S{i + 1}" for i, r in enumerate(keep)}
    for r, es in people.items():
        for e in es:
            e.subject = names.get(r)
    return [names[r] for r in keep]


def _walkable(a: list, b: list, base_mm: float, speed_mm_s: float) -> bool:
    """Whether two time-sorted ``(time_s, position)`` sample lists can be one
    person: at every point where the merged sequence switches from one list
    to the other, the move is within ``base_mm + speed_mm_s * dt``."""
    merged = sorted([(t, p, 0) for t, p in a] + [(t, p, 1) for t, p in b], key=lambda x: x[0])
    for (t0, p0, s0), (t1, p1, s1) in zip(merged, merged[1:], strict=False):
        if s0 != s1 and np.linalg.norm(p1 - p0) > base_mm + speed_mm_s * (t1 - t0):
            return False
    return True


def _join_split_people(people: dict, same_mm: float = 250.0, share: float = 0.8) -> dict:
    """Join two tracks that are one person seen twice at the same ticks.

    Association can split one person into two entries (one per camera) when
    their views do not fit together well enough; both then get tracked, in
    parallel, a few centimetres apart. Two tracks whose heads sit within
    ``same_mm`` of each other on at least ``share`` of the ticks they share
    are one person; :func:`drop_duplicates` later keeps the better entry
    per tick.
    """
    keys = sorted(people, key=lambda r: -len(people[r]))
    pos = {r: {e.tick: e.pose.t for e in people[r]} for r in keys}
    merged: dict = {}
    for r in keys:
        target = None
        for q in merged:
            common = set(pos[r]) & set(pos[q])
            if len(common) < 3:
                continue
            close = sum(np.linalg.norm(pos[r][t] - pos[q][t]) < same_mm for t in common)
            if close >= share * len(common):
                target = q
                break
        if target is None:
            merged[r] = list(people[r])
        else:
            merged[target].extend(people[r])
            pos[target].update({t: p for t, p in pos[r].items() if t not in pos[target]})
    return merged


def drop_duplicates(entries: list[Entry]) -> list[Entry]:
    """At most one entry per subject per tick (the most confident); entries
    without a subject are removed."""
    best: dict = {}
    for e in entries:
        if e.subject is None:
            continue
        key = (e.tick, e.subject)
        if key not in best or e.confidence > best[key].confidence:
            best[key] = e
    return sorted(best.values(), key=lambda e: (e.tick, e.subject))


def _max_gap(t) -> float:
    """Longest pause smoothing and debouncing may bridge: half a second, or
    three analysed samples when analysis runs sparser than that (--skip)."""
    return max(0.5, 3.0 * float(np.median(np.diff(t)))) if len(t) > 1 else 0.5


def finalize(
    entries: list[Entry],
    subjects: list[str],
    times_s,
    regions: list | None = None,
    plane: tuple | None = None,
    smooth: bool = True,
    min_dwell_s: float = 0.15,
) -> None:
    """Smooth each subject's gaze, find what it lands on, debounce the
    labels and mark mutual gaze. Works in place on ``entries``.

    Parameters
    ----------
    entries : list of Entry
        Gaze already computed (:meth:`GazeRig.compute_gaze`), one entry per
        subject per tick (:func:`drop_duplicates`).
    subjects : list of str
        Subject ids from :func:`assign_subjects`.
    times_s : array_like
        Time of every tick, seconds.
    regions, plane
        Targets, see :func:`gaze.targets.cast_gaze`.
    smooth : bool
        Apply :func:`gaze.filters.smooth_series` to origins and directions.
    min_dwell_s : float
        See :func:`gaze.targets.apply_min_dwell`.
    """
    from .filters import smooth_directions, smooth_series
    from .targets import apply_min_dwell, cast_gaze

    times = np.asarray(times_s, dtype=np.float64)
    by_subject = {
        s: sorted((e for e in entries if e.subject == s), key=lambda e: e.tick) for s in subjects
    }
    for e in entries:
        e.raw_direction = e.direction
    if smooth:
        for mine in by_subject.values():
            if len(mine) < 3:
                continue
            t = times[[e.tick for e in mine]]
            gap = _max_gap(t)
            origins = np.array([e.origin for e in mine])
            smoothed = smooth_series(
                origins,
                t,
                np.ones(len(mine), dtype=bool),
                max_gap_s=gap,
                min_cutoff_hz=0.5,
                beta=0.01,
            )
            for e, o in zip(mine, smoothed, strict=True):
                e.origin = o
            has = np.array([e.direction is not None for e in mine])
            if has.sum() >= 3:
                dirs = np.array(
                    [e.direction if e.direction is not None else [0.0, 0.0, 1.0] for e in mine]
                )
                sm = smooth_directions(dirs, t, has, max_gap_s=gap, min_cutoff_hz=1.0, beta=0.3)
                for e, d, ok in zip(mine, sm, has, strict=True):
                    if ok:
                        e.direction = d

    heads_by_tick: dict = {}
    for e in entries:
        heads_by_tick.setdefault(e.tick, {})[e.subject] = e.origin
    for e in entries:
        if e.direction is None:
            e.target = None
            continue
        others = {s: o for s, o in heads_by_tick[e.tick].items() if s != e.subject}
        e.target = cast_gaze(e.origin, e.direction, faces=others, regions=regions, plane=plane)

    # Debounce (type, label) together, so a kept label never pairs with
    # another tick's target type.
    for mine in by_subject.values():
        raw = [(e.target.kind, e.target.label) if e.target is not None else None for e in mine]
        t = times[[e.tick for e in mine]]
        debounced = apply_min_dwell(raw, t, min_dwell_s, max_gap_s=_max_gap(t))
        for e, kept in zip(mine, debounced, strict=True):
            e.label_kind, e.label = kept if kept is not None else (None, None)

    target_of = {(e.tick, e.subject): e for e in entries}
    for e in entries:
        if e.label_kind != "subject":
            e.mutual = False
            continue
        other = target_of.get((e.tick, e.label))
        e.mutual = bool(
            other is not None and other.label_kind == "subject" and other.label == e.subject
        )
