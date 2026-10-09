"""
End-to-end tests of 3D gaze from landmarks onwards: association, head pose,
eye model, fusion, tracking, smoothing, targets and outputs.

The scene is synthetic and exact: two subjects facing each other across a
table, three calibrated cameras, and landmarks made by projecting the real
canonical face (scaled like a real, slightly larger face) with each eye
rotated to look at a known point. The pipeline must recover where each
subject looks, who looks at whom, and that the gaze is mutual.

Needs a downloaded face_landmarker.task (the canonical face lives in it);
skipped otherwise.
"""

import glob
import json
import math
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from gaze.canonical import LEFT_IRIS, RIGHT_IRIS, load_canonical_face
from gaze.eye_model import EYES, IRIS_SPHERE_RADIUS_MM, KAPPA_NASAL_DEG, KAPPA_UP_DEG
from gaze.face_detect import crop_to_frame, cut_crop, landmark_ids, square_crop
from gaze.fusion import (
    FaceObs,
    GazeRig,
    apply_subject_scale,
    assign_subjects,
    drop_duplicates,
    finalize,
)
from gaze.head_pose import HeadPose, project_points
from gaze.ray_math import (
    angle_deg,
    head_direction_from_yaw_pitch,
    unit,
    yaw_pitch_from_head_direction,
)
from gaze.targets import Region

K = np.array([[1300.0, 0.0, 960.0], [0.0, 1300.0, 540.0], [0.0, 0.0, 1.0]])
DIST = np.array([-0.1, 0.05, 0.0, 0.0, 0.0])


def _task_file():
    found = glob.glob(str(Path(__file__).parent.parent / "*" / "models" / "face_landmarker.task"))
    return found[0] if found else None


pytestmark = pytest.mark.skipif(_task_file() is None, reason="no face_landmarker.task downloaded")


def _look_at_camera(position, target):
    """room_from_camera for a camera at `position` looking at `target`
    (camera y roughly down)."""
    z = unit(np.asarray(target, float) - position)
    x = unit(np.cross([0.0, 1.0, 0.0], z))
    y = np.cross(z, x)
    m = np.eye(4)
    m[:3, :3] = np.column_stack([x, y, z])
    m[:3, 3] = position
    return m


def _head_facing(position, towards):
    """Room-from-head rotation for a head at `position` facing `towards`
    (head -z points at it, head +y down)."""
    back = unit(np.asarray(position, float) - towards)  # head +z (into the head)
    x = unit(np.cross([0.0, 1.0, 0.0], back))
    y = np.cross(back, x)
    return np.column_stack([x, y, back])


@pytest.fixture(scope="module")
def scene():
    from pose3d.triangulation import CameraGeom

    canonical = load_canonical_face(_task_file())
    ids = landmark_ids(canonical.rigid_ids)
    # Reference camera at the origin; the room's y axis points down.
    centre = np.array([0.0, 0.0, 2500.0])
    cams = {
        0: CameraGeom(0, K, DIST, np.eye(4)),
        1: CameraGeom(1, K, DIST, _look_at_camera(np.array([-1800.0, 0.0, 1200.0]), centre)),
        2: CameraGeom(2, K, DIST, _look_at_camera(np.array([1800.0, 0.0, 1200.0]), centre)),
    }
    heads = {
        "A": np.array([-700.0, 0.0, 2500.0]),
        "B": np.array([700.0, 0.0, 2500.0]),
    }
    return canonical, ids, cams, heads


def _subject_landmarks(canonical, ids, pose: HeadPose, gaze_target, rig: GazeRig, cam):
    """Pixels of every landmark id for one subject in one camera, with both
    eyes turned to look at `gaze_target`."""
    pos = {int(i): k for k, i in enumerate(ids)}
    pts_room = np.zeros((len(ids), 3))
    for i, k in pos.items():
        if i < 468:
            pts_room[k] = pose.apply(canonical.vertices_mm[i])
    centres = rig.eye_centres_room(pose)
    for name, (_o, _i, _t, _b, nasal) in EYES.items():
        iris_ids = RIGHT_IRIS if name == "right" else LEFT_IRIS
        c = centres[name]
        # The visual axis must point at the target; undo kappa for the
        # optical axis that places the iris.
        want_head = pose.r.T @ unit(gaze_target - c)
        yaw, pitch = yaw_pitch_from_head_direction(want_head)
        optical_head = head_direction_from_yaw_pitch(
            yaw - nasal * math.radians(KAPPA_NASAL_DEG), pitch - math.radians(KAPPA_UP_DEG)
        )
        axis_room = pose.r @ optical_head
        iris = c + IRIS_SPHERE_RADIUS_MM * axis_room
        pts_room[pos[iris_ids[0]]] = iris
        # Rim points 5.85 mm around the iris centre, in the head's x/y.
        for j, off in enumerate([(1, 0), (0, -1), (-1, 0), (0, 1)]):
            pts_room[pos[iris_ids[1 + j]]] = iris + 5.85 * (
                pose.r @ np.array([off[0], off[1], 0.0])
            )
    pc = pts_room @ cam.camera_from_room[:3, :3].T + cam.camera_from_room[:3, 3]
    return project_points(pc, cam.camera_matrix, cam.dist_coeffs)


def _simulate(scene, n_ticks=40, noise_px=0.0, seed=0):
    canonical, ids, cams, heads = scene
    rig = GazeRig(cams, canonical, ids)
    rng = np.random.default_rng(seed)
    scale = 1.06
    poses = {
        s: HeadPose(_head_facing(p, heads["B" if s == "A" else "A"]), p.copy(), scale, 0.0)
        for s, p in heads.items()
    }
    faces_by_tick = {}
    for tick in range(n_ticks):
        per_cam = {}
        for ci, cam in cams.items():
            faces = []
            for s, pose in poses.items():
                other = rig.eye_centres_room(poses["B" if s == "A" else "A"])
                target = (other["right"] + other["left"]) / 2.0
                px = _subject_landmarks(canonical, ids, pose, target, rig, cam)
                px = px + rng.normal(0.0, noise_px, px.shape) if noise_px else px
                if not np.isfinite(px).all():
                    continue
                # Only faces turned towards this camera are visible to it.
                to_cam = unit(cam.extrinsic_rt[:3, 3] - pose.t)
                if np.dot(pose.r @ [0, 0, -1], to_cam) < 0.2:
                    continue
                box = np.array([*px.min(axis=0), *px.max(axis=0)])
                faces.append(FaceObs(camera=ci, box=box, points=px))
            if faces:
                per_cam[ci] = faces
        faces_by_tick[tick] = per_cam
    return rig, poses, faces_by_tick


def _run(rig, faces_by_tick, times, regions=None, plane=None):
    entries = []
    for tick, faces in faces_by_tick.items():
        for group in rig.associate(faces):
            entries.extend(rig.head_pose(group, tick))
    subjects = assign_subjects(entries, times, min_seconds=0.2)
    apply_subject_scale(entries, rig.cameras)
    for e in entries:
        if e.subject is not None:
            rig.compute_gaze(e)
    entries = drop_duplicates(entries)
    finalize(entries, subjects, times, regions=regions, plane=plane)
    return entries, subjects


def test_two_subjects_looking_at_each_other(scene):
    rig, poses, faces = _simulate(scene)
    times = np.arange(len(faces)) / 25.0
    entries, subjects = _run(rig, faces, times)
    assert subjects == ["S1", "S2"]  # named left to right: A is S1
    for e in entries:
        truth = poses["A"] if e.subject == "S1" else poses["B"]
        other = poses["B"] if e.subject == "S1" else poses["A"]
        c = rig.eye_centres_room(other)
        target = (c["right"] + c["left"]) / 2.0
        # The gaze ray starts at (or between) this subject's eyes and passes
        # through the point they look at.
        eyes = rig.eye_centres_room(truth)
        assert (
            min(
                np.linalg.norm(e.origin - p)
                for p in (eyes["right"], eyes["left"], (eyes["right"] + eyes["left"]) / 2)
            )
            < 2.0
        )
        assert angle_deg(e.direction, target - e.origin) < 0.3
        assert e.pose.scale == pytest.approx(1.06, abs=0.01)
        assert e.label_kind == "subject" and e.label == ("S2" if e.subject == "S1" else "S1")
        assert e.mutual
        assert e.n_cameras >= 2


def test_landmark_noise_keeps_the_target_right(scene):
    rig, _poses, faces = _simulate(scene, noise_px=0.6, seed=3)
    times = np.arange(len(faces)) / 25.0
    entries, _ = _run(rig, faces, times)
    labelled = [e for e in entries if e.label_kind == "subject"]
    assert len(labelled) >= 0.9 * len(entries)
    assert np.mean([e.mutual for e in entries]) > 0.8
    assert all(e.uncertainty_deg is not None and e.uncertainty_deg < 15 for e in entries)


def test_a_region_in_front_of_the_face_wins(scene):
    rig, poses, faces = _simulate(scene, n_ticks=10)
    times = np.arange(len(faces)) / 25.0
    # A sheet of glass between the two subjects, crossing both gaze lines.
    glass = Region("glass", [0.0, 0.0, 2500.0], [1.0, 0.0, 0.0], [0.0, 0.0, 1.0], 600.0, 600.0)
    entries, _ = _run(rig, faces, times, regions=[glass])
    assert all(e.label == "glass" and e.label_kind == "region" for e in entries)
    assert not any(e.mutual for e in entries)
    for e in entries:
        assert abs(e.target.point[0]) < 1.0  # on the glass plane x = 0


def test_single_camera_subject_uses_its_learned_face_scale(scene):
    rig, poses, faces = _simulate(scene, n_ticks=20)
    # Second half: only the reference camera sees anyone.
    for tick in range(10, 20):
        faces[tick] = {0: faces[tick][0]} if 0 in faces[tick] else {}
    times = np.arange(len(faces)) / 25.0
    entries, subjects = _run(rig, faces, times)
    single = [e for e in entries if e.n_cameras == 1]
    assert single, "expected single-camera entries"
    for e in single:
        truth = poses["A"] if e.subject == "S1" else poses["B"]
        assert e.pose.scale == pytest.approx(1.06, abs=0.01)
        assert np.linalg.norm(e.pose.t - truth.t) < 30.0  # depth right, thanks to the scale


# ── Crops and outputs ────────────────────────────────────────────────────────


def test_crop_landmarks_map_back_to_the_frame():
    frame = np.zeros((100, 200, 3), dtype=np.uint8)
    frame[40, 150] = 255
    x0, y0, side = square_crop([140, 30, 170, 60], factor=2.0)
    crop = cut_crop(frame, x0, y0, side)
    yy, xx = np.argwhere(crop[..., 0] == 255)[0]
    back = crop_to_frame([[(xx + 0.0) / side, (yy + 0.0) / side]], x0, y0, side)[0]
    assert np.allclose(back, [150, 40])
    # A crop off the frame edge is padded, not shifted.
    x0, y0, side = square_crop([180, 80, 200, 100], factor=2.0)
    assert x0 + side > 200 and cut_crop(frame, x0, y0, side).shape[:2] == (side, side)


def test_outputs_are_written_and_consistent(scene, tmp_path):
    from gaze.io_v2 import summarise, write_csv, write_results
    from gaze.timeline import CameraTrack, Timeline

    rig, _poses, faces = _simulate(scene, n_ticks=12)
    times = np.arange(len(faces)) / 25.0
    entries, subjects = _run(rig, faces, times)
    tl = Timeline(
        "synced",
        (times * 1e9).astype(np.int64),
        25.0,
        {
            i: CameraTrack(i, tmp_path / f"video_{i}.mp4", np.arange(12), np.zeros(12, bool))
            for i in rig.cameras
        },
    )
    out = write_results(
        tmp_path,
        tl,
        entries,
        list(range(12)),
        subjects,
        {"S1": "Child"},
        rig.cameras,
        None,
        [],
        1,
        {},
        None,
    )
    data = json.loads(out.read_text())
    assert data["schema"] == "mosaic-gaze-fusion-v2"
    assert [s["name"] for s in data["subjects"]] == ["Child", "S2"]
    first = data["frames"][0]["subjects"]
    s2 = next(s for s in first if s["id"] == "S2")
    assert s2["target"] == {
        "type": "subject",
        "label": "Child",
        "subject": "S1",
        "distance_mm": pytest.approx(s2["target"]["distance_mm"]),
        "measured": None,
    }
    assert s2["mutual"] is True
    write_csv(tmp_path / "g.csv", tl, entries, {"S1": "Child"})
    rows = (tmp_path / "g.csv").read_text().splitlines()
    assert rows[0].startswith("tick,timestamp_ns,video_frame_index,subject")
    assert len(rows) == 1 + len(entries)
    summary = summarise(entries, subjects, {"S1": "Child"}, 1 / 25.0, 12, 1)
    assert summary["subjects"]["Child"]["looked_at_s"]["S2"] == pytest.approx(12 / 25.0)
    assert summary["mutual_gaze"]["Child & S2"]["pct_of_recording"] == pytest.approx(100.0)


def test_neighbours_at_camera_height_stay_two_people(scene):
    # Two people 600 mm apart, all heads and cameras at one height: every
    # pair of sight lines meets somewhere, so triangulation alone matched
    # them. Each sight line's own depth band keeps them apart.
    canonical, ids, cams, _heads = scene
    near = {"A": np.array([-300.0, 0.0, 2600.0]), "B": np.array([300.0, 0.0, 2600.0])}
    rig, _poses, faces = _simulate((canonical, ids, cams, near), n_ticks=20)
    times = np.arange(len(faces)) / 25.0
    entries, subjects = _run(rig, faces, times)
    assert subjects == ["S1", "S2"]
    for e in entries:
        assert e.pose.rms_px < 2.0  # never a compromise between two heads
