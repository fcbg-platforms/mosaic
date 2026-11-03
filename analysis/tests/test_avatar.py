"""3D Avatar (avatar/, run_avatar.py): reading the 3D Pose people, the plain
geometry, the file format, the 3D hands given to people, and (with Anny
installed) a fit of a body and of hands Anny posed itself, and the exported
file skinned back."""

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import run_avatar
from avatar import io
from avatar.fit import (
    HAND_GAP_MS,
    HAND_HINGES,
    HAND_HOLD_MS,
    HAND_POINTS,
    HAND_SPREAD,
    carry_angles,
)
from avatar.hands import assign, load_hands, nearest_ticks
from avatar.people import (
    chunks,
    load_people,
    nearest_sample,
    smooth_mask,
    up_frame,
    usable,
    valid_bits,
)


def _skeleton(frames):
    return {"frames": frames}


def _person(pid, pts, track=0):
    return {"person_id": pid, "track_id": track, "keypoints_room_smoothed": pts}


def test_load_people_groups_tracks_by_person_and_converts_to_metres():
    pts = [[1000.0, 2000.0, 3000.0]] * 16 + [None]
    sk = _skeleton(
        [
            {"tick": 0, "timestamp_ns": 5_000_000_000, "people": [_person(1, pts, track=0)]},
            {"tick": 1, "timestamp_ns": 5_040_000_000, "people": [_person(1, pts, track=3)]},
            {"tick": 1, "timestamp_ns": 5_040_000_000, "people": [_person(1, pts, track=4)]},
            {"tick": 2, "timestamp_ns": 5_080_000_000, "people": [_person(2, pts)]},
        ]
    )
    people = load_people(sk)
    assert sorted(people) == [1, 2]
    one = people[1]
    assert one["ticks"].tolist() == [0, 1]  # one tick each, the second track left out
    assert one["time_ms"].tolist() == [5000.0, 5040.0]
    assert one["keypoints"].shape == (2, 17, 3)
    assert one["keypoints"][0, 0].tolist() == [1.0, 2.0, 3.0]
    assert np.isnan(one["keypoints"][0, 16]).all()
    # A result from before people were joined: tracks only, numbered from 1
    # as MOSAIC's 3D Pose reader numbers them, so the views find the body.
    old = {"track_id": 0, "keypoints_room": pts}
    assert sorted(load_people(_skeleton([{"tick": 0, "timestamp_ns": 0, "people": [old]}]))) == [1]


def test_up_frame_points_z_from_the_feet_to_the_shoulders():
    kp = np.zeros((3, 17, 3))
    kp[:, 5] = kp[:, 6] = (0.2, -1.5, 3.0)  # shoulders: room y is down
    kp[:, 15] = kp[:, 16] = (0.2, 0.0, 3.0)  # ankles
    M = up_frame(kp)
    assert np.allclose(M @ M.T, np.eye(3), atol=1e-9) and np.linalg.det(M) == pytest.approx(1)
    assert M[2] == pytest.approx([0, -1, 0])
    assert M @ np.array([0, -1.0, 0]) == pytest.approx([0, 0, 1])
    # Nobody's ankles seen (all behind a desk): from the hips.
    hidden = kp.copy()
    hidden[:, 15:] = np.nan
    hidden[:, 11] = hidden[:, 12] = (0.2, -0.9, 3.0)
    assert up_frame(hidden)[2] == pytest.approx([0, -1, 0])
    # Neither: the room's -y. And the room's x nearly up: its z made level.
    assert up_frame(np.full((2, 17, 3), np.nan))[2] == pytest.approx([0, -1, 0])
    sideways = np.zeros((1, 17, 3))
    sideways[:, 5] = sideways[:, 6] = (1.5, 0.0, 0.01)
    M = up_frame(sideways)
    assert np.allclose(M @ M.T, np.eye(3), atol=1e-9) and abs(M[0] @ [1, 0, 0]) < 0.1


def test_smooth_mask_and_chunks():
    assert smooth_mask(np.array([0, 1, 2, 3, 10, 11, 12])).tolist() == [
        True, True, False, False, True,
    ]  # fmt: skip
    assert chunks(50, 100, 10) == [(0, 50, 0, 50)]
    parts = chunks(250, 100, 20)
    assert parts[0][:2] == (0, 100) and parts[-1][1] == 250
    kept = [i for _a, _b, k0, k1 in parts for i in range(k0, k1)]
    assert kept == list(range(250))  # every tick kept once, from inside a piece
    assert all(a <= k0 < k1 <= b for a, b, k0, k1 in parts)


def test_usable_ticks_and_the_nearest_sample():
    v = np.ones((4, 17), bool)
    v[1, 11] = False  # a hip missing
    v[2, 6] = False  # a shoulder missing
    v[3, :10] = False  # 7 joints
    assert usable(v).tolist() == [True, False, False, False]
    ticks = np.array([0, 1, 2, 3, 10, 11, 20])
    sample = np.array([0, 4, 6])  # ticks 0, 10, 20
    assert nearest_sample(ticks, sample).tolist() == [0, 0, 0, 0, 1, 1, 2]
    assert nearest_sample(ticks, np.array([3])).tolist() == [0] * 7


def test_valid_bits():
    kp = np.zeros((2, 17, 3))
    kp[0, 3] = np.nan
    kp[1, 16, 2] = np.nan
    bits = valid_bits(kp)
    assert bits[0] == (1 << 17) - 1 - (1 << 3) and bits[1] == (1 << 16) - 1


def _hands_file(path, hands_per_frame):
    """A hands3d.frames.json: per frame (40 ms apart from 5 s) its hands,
    each (hand, side, wrist in mm): the wrist at the wrist, the other points
    a hand's length in front."""
    frames = []
    for i, hands in enumerate(hands_per_frame):
        frames.append(
            {
                "timestamp_ms": 5000.0 + 40.0 * i,
                "hands": [
                    {
                        "hand": n,
                        "side": side,
                        "points_mm": [list(w)] + [[w[0], w[1], w[2] - 100.0]] * 19 + [None],
                    }
                    for n, side, w in hands
                ],
            }
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"frames": frames}))


def test_hands_go_to_the_person_whose_wrist_is_theirs(tmp_path):
    # Person 1's left wrist at x = 0, person 2's at x = 2 m (index 9: left
    # wrist; 10: right wrist, 0.4 m to the side).
    def person(x):
        kp = np.full((4, 17, 3), np.nan)
        kp[:, 9] = [x, -1.0, 3.0]
        kp[:, 10] = [x + 0.4, -1.0, 3.0]
        return {"ticks": np.arange(4), "time_ms": 5000.0 + 40.0 * np.arange(4), "keypoints": kp}

    people = {1: person(0.0), 2: person(2.0)}
    path = tmp_path / "hands" / "hands3d.frames.json"
    near1, near2 = (30.0, -1000.0, 3000.0), (2010.0, -1000.0, 3000.0)
    _hands_file(
        path,
        [
            # Hand 7, a left hand at person 1's left wrist; hand 8 at person
            # 2's, but a "right" hand there is nobody's (their right wrist is
            # 0.4 m away); hand 9, a second left hand near person 1, in the
            # same frame as hand 7 but further from the wrist.
            [(7, "left", near1), (8, "right", near2), (9, "left", (60.0, -1000.0, 3000.0))],
            [(7, "left", near1)],
            [(7, "left", near1), (5, "left", near2)],
            [(7, "left", (400.0, -1000.0, 3000.0))],  # hand 7, away from the wrist
        ],
    )
    hands = load_hands(path)
    assert sorted(hands) == [5, 7, 8, 9] and hands[7]["points"].shape == (4, 21, 3)
    assert hands[7]["points"][0, 0].tolist() == [0.03, -1.0, 3.0]  # metres
    assert np.isnan(hands[7]["points"][0, 20]).all()
    got = assign(hands, people)
    assert sorted(got) == [1, 2] and sorted(got[1]) == ["left"] and sorted(got[2]) == ["left"]
    one = got[1]["left"]
    assert one["time_ms"].tolist() == [5000.0, 5040.0, 5080.0]  # not its far frame
    assert one["points"][0, 0].tolist() == [0.03, -1.0, 3.0]  # hand 7, the nearer, kept
    assert load_hands(tmp_path / "none.json") == {}
    assert nearest_ticks(np.array([0.0, 40.0]), np.array([15.0, 25.0, 90.0]), 20.0).tolist() == [
        0,
        1,
        -1,
    ]


def test_carry_angles_between_hand_frames():
    time_ms = np.arange(30) * 40.0
    ticks = np.array([5, 10, 25])  # 200 ms apart, then 600 ms (over HAND_GAP_MS)
    assert time_ms[25] - time_ms[10] > HAND_GAP_MS
    angles = {"flex": np.array([[0.0], [1.0], [3.0]])}
    out = carry_angles(angles, ticks, time_ms, 30)
    assert out["flex"][5, 0] == 0.0 and out["flex"][10, 0] == 1.0
    assert out["flex"][7, 0] == pytest.approx(0.4)  # between the first two
    assert out["on"][5:11].all()
    hold = int(HAND_HOLD_MS // 40)  # a tick or so either side of a lone frame
    assert out["on"][25 - hold : 26 + hold].all() and out["flex"][24, 0] == 3.0
    assert not out["on"][15] and out["flex"][15, 0] == 0.0  # in the long gap: none
    assert not out["on"][0] and not out["on"][29]


def _arrays(n=4, v=5, j=3, k=2):
    rng = np.random.default_rng(0)
    return {
        "rest_vertices": rng.normal(size=(v, 3)).astype(np.float32),
        "faces": np.array([[0, 1, 2], [2, 3, 4]], np.uint32),
        "skin_bones": rng.integers(0, j, size=(v, k)).astype(np.uint16),
        "skin_weights": np.full((v, k), 1.0 / k, np.float32),
        "time_ms": np.arange(n) * 40.0 + 5000.0,
        "valid": np.arange(n, dtype=np.uint32),
        "bones": rng.normal(size=(n, j, 7)).astype(np.float32),
    }


def test_person_file_round_trip(tmp_path):
    arrays = _arrays()
    counts = io.write_person(tmp_path / "person_1.bin", arrays)
    assert counts == {
        "vertices": 5, "faces": 2, "weights_per_vertex": 2, "ticks": 4, "bone_count": 3,
        "bytes": 5 * 12 + 2 * 12 + 5 * 2 * 2 + 5 * 2 * 4 + 4 * 8 + 4 * 4 + 4 * 3 * 7 * 4,
    }  # fmt: skip
    back = io.read_person(tmp_path / "person_1.bin", counts)
    for name, value in arrays.items():
        assert np.array_equal(back[name], value.astype(back[name].dtype)), name
    assert not (tmp_path / "person_1.bin.partial").exists()
    with pytest.raises(ValueError):
        io.read_person(tmp_path / "person_1.bin", {**counts, "ticks": 3})


class _FakeFitter:
    """Stands in for Anny: the fitted keypoints are the targets, and the
    file has a 3-bone, 5-vertex body."""

    labels = ["root", "lowerarm01.L", "wrist.L"]

    def fit(self, keypoints_zup, ticks, log=print):
        n = len(ticks)
        return {
            "phenotype": {"age": 0.25, "height": 0.4},
            "hip_offset_m": np.zeros((2, 3)),
            "error_m": np.where(np.isfinite(keypoints_zup).all(-1), 0.02, np.nan),
            "n": n,
        }

    def fit_hands(self, fit, hands, time_ms, log=print):
        self.hands_seen = hands
        n = len(time_ms)
        return {
            side: {"frames": 2, "on": np.ones(n, bool), "error_m": np.full((2, 20), 0.004)}
            for side in hands
        }

    def export(self, fit):
        a = _arrays(n=fit["n"])
        del a["time_ms"], a["valid"]
        return a


def test_runner_writes_the_avatar_json_and_one_file_per_person(tmp_path, capsys):
    s = tmp_path / "sess"
    s.mkdir()
    pts = [[0.0, -1500.0, 3000.0]] * 7 + [[0.0, -1000.0, 3000.0]] * 8 + [[0.0, 0.0, 3000.0]] * 2
    frames = [
        {
            "tick": t,
            "timestamp_ns": int(5e9 + 4e7 * t),
            "people": [_person(1, pts), _person(2, pts)],
        }
        for t in range(6)
    ]
    (s / "skeleton3d.json").write_text(json.dumps(_skeleton(frames)))
    assert run_avatar.main(["--session", str(s), "--person", "2"], fitter=_FakeFitter()) == 0
    res = json.loads((s / "avatar" / "avatar.json").read_text())
    assert res["schema"] == "mosaic-avatar-v1" and res["body_model"] == "anny"
    assert len(res["keypoint_names"]) == 17
    assert res["bone_labels"] == ["root", "lowerarm01.L", "wrist.L"]
    (p,) = res["people"]
    assert p["person_id"] == 2 and p["file"] == "avatar/person_2.bin" and p["ticks"] == 6
    assert p["median_error_mm"] == 20.0 and p["per_joint_median_error_mm"]["nose"] == 20.0
    assert p["p90_error_mm"] == 20.0 and p["per_joint_p90_error_mm"]["right_ankle"] == 20.0
    assert res["failed"] == []
    assert p["phenotype"] == {"age": 0.25, "height": 0.4} and p["seconds"] == 0.2
    back = io.read_person(s / p["file"], p)
    assert back["time_ms"].tolist() == [5000.0 + 40 * t for t in range(6)]
    assert back["valid"][0] == (1 << 17) - 1
    to_room = np.array(res["to_room"])
    assert to_room @ np.array([0, 0, 1.0]) == pytest.approx([0, -1000, 0])  # z up is room -y
    assert "Step 1/1: person 2, 6 ticks" in capsys.readouterr().out
    assert not (s / "avatar" / "person_1.bin").exists()
    # Fitting person 1 as well keeps person 2's result...
    assert run_avatar.main(["--session", str(s), "--person", "1"], fitter=_FakeFitter()) == 0
    res = json.loads((s / "avatar" / "avatar.json").read_text())
    assert [p["person_id"] for p in res["people"]] == [1, 2]
    # ...and a full run starts over: an earlier person's file is removed.
    (s / "avatar" / "person_9.bin").write_bytes(b"old")
    assert run_avatar.main(["--session", str(s)], fitter=_FakeFitter()) == 0
    assert not (s / "avatar" / "person_9.bin").exists()
    assert len(json.loads((s / "avatar" / "avatar.json").read_text())["people"]) == 2


def test_runner_leaves_out_people_with_too_few_joints(tmp_path, capsys):
    s = tmp_path / "sess"
    s.mkdir()
    full = [[0.0, -1500.0, 3000.0]] * 7 + [[0.0, -1000.0, 3000.0]] * 8 + [[0.0, 0.0, 3000.0]] * 2
    no_hips = [p if k not in (11, 12) else None for k, p in enumerate(full)]
    frames = [
        {
            "tick": t,
            "timestamp_ns": int(5e9 + 4e7 * t),
            "people": [_person(1, full), _person(2, no_hips)],
        }
        for t in range(4)
    ]
    (s / "skeleton3d.json").write_text(json.dumps(_skeleton(frames)))
    assert run_avatar.main(["--session", str(s)], fitter=_FakeFitter()) == 0
    res = json.loads((s / "avatar" / "avatar.json").read_text())
    assert [p["person_id"] for p in res["people"]] == [1] and res["failed"] == [2]
    assert "person 2: too few joints" in capsys.readouterr().out


def test_runner_needs_3d_pose(tmp_path):
    assert run_avatar.main(["--session", str(tmp_path)], fitter=_FakeFitter()) == 1


def test_runner_fits_the_fingers_of_the_person_with_3d_hands(tmp_path, capsys):
    s = tmp_path / "sess"
    s.mkdir()
    full = [[0.0, -1500.0, 3000.0]] * 7 + [[0.0, -1000.0, 3000.0]] * 8 + [[0.0, 0.0, 3000.0]] * 2
    far = [[x + 2000.0, y, z] for x, y, z in full]
    frames = [
        {
            "tick": t,
            "timestamp_ns": int(5e9 + 4e7 * t),
            "people": [_person(1, full), _person(2, far)],
        }
        for t in range(5)
    ]
    (s / "skeleton3d.json").write_text(json.dumps(_skeleton(frames)))
    # A left hand at person 1's left wrist (3D Pose's index 9).
    _hands_file(s / "hands" / "hands3d.frames.json", [[(1, "left", (0.0, -990.0, 3000.0))]] * 3)
    f = _FakeFitter()
    assert run_avatar.main(["--session", str(s)], fitter=f) == 0
    res = json.loads((s / "avatar" / "avatar.json").read_text())
    one, two = res["people"]
    assert one["hands"] == {
        "left": {"frames": 2, "seconds": 0.2, "median_error_mm": 4.0, "p90_error_mm": 4.0}
    }
    assert "hands" not in two
    # In the fit's frame: z up, from the room's -y; metres.
    (side,) = f.hands_seen
    assert side == "left" and f.hands_seen["left"]["points"][0, 0, 2] == pytest.approx(0.99)
    assert "person 1: 3D hands, left" in capsys.readouterr().out


@pytest.fixture(scope="module")
def fitter():
    pytest.importorskip("anny")
    torch = pytest.importorskip("torch")
    from avatar import fit as F

    F.SHAPE_ITERS, F.POSE_ITERS = 300, 400  # a short body: enough to recover it
    return F.AnnyFitter("cuda" if torch.cuda.is_available() else "cpu", log=lambda m: None)


def test_anny_fit_recovers_a_body_anny_posed(fitter):
    """Keypoints of Anny's own body, walking-ish, are fitted back: the fitted
    keypoints land on them, and the exported file skins back to them."""
    import torch

    f = fitter
    n = 30
    t = torch.linspace(0, 1, n, device=f.dev)
    p = {
        "rot": torch.zeros(n, len(f.free), 3, device=f.dev),
        "bend": torch.full((n, len(f.hinge)), -1.0, device=f.dev),
        "trans": torch.stack([t * 0.5, t * 0, t * 0 + 0.1], 1),
    }
    p["bend"][:, 2:] = 0.5 * torch.sin(5 * t)[:, None]  # the elbows bend and straighten
    p["rot"][:, 1, 0] = 0.4 * torch.sin(6 * t)  # the left thigh swings
    p["rot"][:, 0, 2] = 0.3  # turned a little
    pheno = torch.zeros(6, device=f.dev)
    with torch.no_grad():
        Q, rbp = f.keypoint_basis(torch.sigmoid(pheno))
        hinges = f.hinges(Q, rbp)
        kp, _ = f._keypoints(p, Q, rbp, hinges, torch.zeros(2, 3, device=f.dev))
    target = kp.cpu().numpy()
    target[5, 3] = np.nan  # a joint not found
    fit = f.fit(target, np.arange(n))
    err = fit["error_m"][np.isfinite(fit["error_m"])]
    assert np.median(err) < 0.02  # within 2 cm
    assert all(0.0 <= v <= 1.0 for v in fit["phenotype"].values())
    a = f.export(fit)
    assert a["bones"].shape == (n, f.J, 7)
    assert np.allclose(np.linalg.norm(a["bones"][..., :4], axis=-1), 1.0, atol=1e-4)
    # Skinning the file by hand gives the same vertices as Anny.
    k = 12
    q, tr = a["bones"][k, :, :4], a["bones"][k, :, 4:]
    x, y, z, w = q.T
    R = np.stack(
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
         2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
         2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)], -1,
    ).reshape(-1, 3, 3)  # fmt: skip
    per_bone = np.einsum("jab,vb->vja", R, a["rest_vertices"]) + tr[None]
    sel = per_bone[np.arange(len(per_bone))[:, None], a["skin_bones"]]
    mine = (sel * a["skin_weights"][..., None]).sum(1)
    with torch.no_grad():
        bend = f._bend(fit["params"]["bend"][k : k + 1], fit["hinges"])
        pose = f.pose_matrices(fit["params"]["rot"][k : k + 1], bend, fit["hinges"]["axes"])
        out = f.model(
            pose_parameters=pose,
            phenotype_kwargs=f._phenotype_kwargs(torch.sigmoid(fit["pheno"])),
        )
        theirs = (out["vertices"][0] + fit["params"]["trans"][k]).cpu().numpy()
    assert np.abs(mine - theirs).max() < 1e-3  # within a millimetre


def test_anny_hinges_straighten_and_bend_the_right_way(fitter):
    """Elbows are bent in Anny's reference pose: they can still straighten
    (their range starts below zero), and a bend brings the wrist towards the
    shoulder in the arm's own plane; knees bend backwards."""
    import torch

    f = fitter
    with torch.no_grad():
        Q, rbp = f.keypoint_basis(torch.full((6,), 0.0, device=f.dev))
        h = f.hinges(Q, rbp)
        eye = torch.eye(4, device=f.dev)[None, None].repeat(1, f.J, 1, 1)
        ref = f._kp_at(eye, Q, rbp)[0]
    lo, hi = h["lo"].cpu().numpy(), h["hi"].cpu().numpy()
    assert (lo[2:] < -np.deg2rad(10)).all()  # the elbows: from straight, below their reference
    assert hi - lo == pytest.approx(np.deg2rad(150.0))
    from avatar.fit import HINGES

    for i, (bone, (far, mid, near)) in enumerate(HINGES.items()):
        for bend, closer in ((float(lo[i]), False), (float(lo[i]) + 0.8, True)):
            with torch.no_grad():
                b = torch.zeros(1, 4, device=f.dev)
                b[0, i] = bend
                pose = f.pose_matrices(torch.zeros(1, len(f.free), 3, device=f.dev), b, h["axes"])
                kp = f._kp_at(pose, Q, rbp)[0]
            reach = float(torch.linalg.norm(kp[far] - kp[near]))
            reach_ref = float(torch.linalg.norm(ref[far] - ref[near]))
            if closer:
                assert reach < reach_ref, bone
            else:  # straight: the limb at full length
                full = float(
                    torch.linalg.norm(kp[mid] - kp[near]) + torch.linalg.norm(kp[far] - kp[mid])
                )
                assert reach == pytest.approx(full, rel=0.02), bone
        if "leg" in bone:
            with torch.no_grad():
                b = torch.zeros(1, 4, device=f.dev)
                b[0, i] = float(lo[i]) + 1.0
                pose = f.pose_matrices(torch.zeros(1, len(f.free), 3, device=f.dev), b, h["axes"])
                kp = f._kp_at(pose, Q, rbp)[0]
            assert float(kp[far, 1] - ref[far, 1]) > 0, bone  # backwards (Anny faces -y)


def _straight_body(f, n):
    import torch

    pheno = torch.zeros(6, device=f.dev)
    with torch.no_grad():
        Q, rbp = f.keypoint_basis(torch.sigmoid(pheno))
        hinges = f.hinges(Q, rbp)
    params = {
        "rot": torch.zeros(n, len(f.free), 3, device=f.dev),
        "bend": torch.full((n, len(f.hinge)), -1.0, device=f.dev),
        "trans": torch.zeros(n, 3, device=f.dev),
    }
    with torch.no_grad():
        body = f.pose_matrices(params["rot"], f._bend(params["bend"], hinges), hinges["axes"])
    return {"pheno": pheno, "params": params, "hinges": hinges}, body, rbp


def test_anny_fingers_bend_towards_the_palm(fitter):
    """Bending every finger joint brings each fingertip towards the wrist,
    on both hands; straightening the knuckles past the reference does not."""
    import torch

    f = fitter
    fit, body, rbp = _straight_body(f, 1)
    with torch.no_grad():
        basis = f.hand_basis(torch.sigmoid(fit["pheno"]), rbp)
    for side in ("left", "right"):
        b = basis[side]
        ref = f.hand_points(body, rbp, b)[0]

        def tips_to_wrist(flex, b=b):
            a = {
                "rv": torch.zeros(1, 2, 3, device=f.dev),
                "flex": torch.full((1, len(HAND_HINGES)), flex, device=f.dev),
                "spread": torch.zeros(1, 4, device=f.dev),
            }
            with torch.no_grad():
                p = f.hand_points(f.hand_pose(body, a, b), rbp, b)[0]
            return torch.linalg.norm(p[[8, 12, 16, 20]] - p[0], dim=-1)

        reach = torch.linalg.norm(ref[[8, 12, 16, 20]] - ref[0], dim=-1)
        assert (tips_to_wrist(0.8) < reach - 0.03).all(), side  # a fist: 3 cm nearer at least
        assert (tips_to_wrist(-0.05) >= reach - 0.002).all(), side


def test_anny_finger_joints_bend_in_the_hands_own_planes(fitter):
    """Each joint's bend takes the next point towards the palm, in the plane
    of the bone and the palm's normal (hardly along the line across them);
    a knuckle's spread takes it along that line, across the hand. (Anny
    turns bones about axes fixed in the room, so its bones' frame axes are
    not the hand's.)"""
    import roma
    import torch

    f = fitter
    fit, body, rbp = _straight_body(f, 1)
    with torch.no_grad():
        basis = f.hand_basis(torch.sigmoid(fit["pheno"]), rbp)
    for side in ("left", "right"):
        b = basis[side]
        with torch.no_grad():
            ref = f.hand_points(body, rbp, b)[0]
        n = torch.linalg.cross(ref[5] - ref[0], ref[17] - ref[0])
        palm = n / torch.linalg.norm(n) * torch.sign(torch.dot(ref[4] - ref[0], n))
        for i, name in enumerate(HAND_HINGES):
            head = HAND_POINTS.index(name)
            child = head + 1
            across = torch.linalg.cross(ref[child] - ref[head], palm)
            across = across / torch.linalg.norm(across)
            for axis, spread in [(b["flex_axes"][i], False)] + (
                [(b["spread_axes"][HAND_SPREAD.index(name)], True)] if name in HAND_SPREAD else []
            ):
                pose = body.clone()
                bone = b["hinge"][i]
                pose[0, bone, :3, :3] = roma.rotvec_to_rotmat(0.5 * axis) @ pose[0, bone, :3, :3]
                with torch.no_grad():
                    m = f.hand_points(pose, rbp, b)[0, child] - ref[child]
                towards, sideways = float(m @ palm), abs(float(m @ across))
                if spread:
                    assert sideways > 0.003 and abs(towards) < 0.3 * sideways, (side, name)
                else:
                    assert towards > 0.003 and sideways < 0.1 * float(torch.linalg.norm(m)), (
                        side,
                        name,
                    )


def test_anny_fingers_recover_a_hand_anny_posed(fitter):
    """A hand Anny posed itself (closing and opening, the wrist turning),
    seen every 200 ms with 3 mm of noise, is fitted back: its points within
    the noise, and fingers at the ticks between hand frames."""
    import torch

    f = fitter
    n = 40
    fit, body, rbp = _straight_body(f, n)
    with torch.no_grad():
        b = f.hand_basis(torch.sigmoid(fit["pheno"]), rbp)["left"]
    t = torch.linspace(0, 1, n, device=f.dev)
    nh = len(HAND_HINGES)
    truth = {
        "rv": torch.zeros(n, 2, 3, device=f.dev),
        "flex": (0.6 + 0.5 * torch.sin(4 * t))[:, None]
        * torch.linspace(0.5, 1.0, nh, device=f.dev)[None],
        "spread": 0.1 * torch.cos(3 * t)[:, None].repeat(1, 4),
    }
    truth["rv"][:, 0, 0] = 0.3 * torch.sin(5 * t)
    with torch.no_grad():
        pts = f.hand_points(f.hand_pose(body, truth, b), rbp, b).cpu().numpy()
    time_ms = np.arange(n) * 40.0
    seen = np.arange(0, n, 5)
    noisy = pts[seen] + 0.003 * np.random.default_rng(0).standard_normal((len(seen), 21, 3))
    noisy[2, 7] = np.nan  # a point not found
    noisy[3, 9] = np.nan  # a knuckle not found: the frame is left out
    res = f.fit_hands(fit, {"left": {"time_ms": time_ms[seen], "points": noisy}}, time_ms)
    assert set(res) == {"basis", "left"}
    h = res["left"]
    assert h["frames"] == len(seen) - 1 and np.isnan(h["error_m"][2, 6])
    assert np.nanmedian(h["error_m"]) < 0.008
    assert h["on"][: seen[-1] + 1].all()
    # The exported file has the fingers: the hand's bones turned, the
    # fingertips where the fit put them.
    fit["hands"] = res
    a = f.export(fit)
    tip = b["riders"][8].item()
    assert not np.allclose(a["bones"][10, tip, :4], [0, 0, 0, 1], atol=1e-3)
    with torch.no_grad():
        k = 10
        mine = {
            key: torch.tensor(h[key][k : k + 1], device=f.dev) for key in ("rv", "flex", "spread")
        }
        p = f.hand_points(f.hand_pose(body[k : k + 1], mine, b), rbp, b)[0].cpu().numpy()
    err = np.linalg.norm(
        (p - p[0])[[4, 8, 12, 16, 20]] - (pts[k] - pts[k, 0])[[4, 8, 12, 16, 20]], axis=1
    )
    assert err.max() < 0.012  # each fingertip within about a centimetre of the truth
