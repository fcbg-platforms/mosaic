"""Anny fitted to a person's 3D Pose keypoints (torch; see docs/math/avatar.rst).

Two stages. The body shape (Anny's phenotype: gender, age, muscle, weight,
height, proportions) and a fixed offset of the COCO hip points are found on a
sample of ticks; then every tick's pose, with the shape fixed, in chunks,
smooth over time. The pose is the root's rotation and position and the
rotations of the main body bones; knees and elbows are hinges (one bend
angle each, from straight to 150 degrees) so they never bend the wrong way.
A robust (Huber) loss keeps one bad 3D Pose joint from pulling the body.

With the shape fixed, each COCO keypoint is a fixed combination of the bone
transforms (the keypoints are fixed sums of vertices, and the vertices fixed
sums of bone transforms: linear blend skinning), so the fit needs no
skinning in its loop.
"""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np

from avatar.people import L_HIP, R_HIP, chunks, nearest_sample, smooth_mask, usable

CACHE_DIR = Path(__file__).parent / "cache"
#: Bones posed freely (a rotation each); the rest stay at rest.
FREE_BONES = (
    "root", "upperleg01.L", "upperleg02.L", "foot.L", "upperleg01.R", "upperleg02.R", "foot.R",
    "spine05", "spine04", "spine03", "spine02", "spine01",
    "clavicle.L", "shoulder01.L", "upperarm01.L", "wrist.L",
    "clavicle.R", "shoulder01.R", "upperarm01.R", "wrist.R",
    "neck01", "neck02", "head",
)  # fmt: skip
#: Hinges, with the COCO keypoints at their far end, at the joint, and at
#: the near end (ankle, knee, hip; wrist, elbow, shoulder).
HINGES = {
    "lowerleg01.L": (15, 13, 11),
    "lowerleg01.R": (16, 14, 12),
    "lowerarm01.L": (9, 7, 5),
    "lowerarm01.R": (10, 8, 6),
}
#: A limb bends from straight to this, in all.
MAX_BEND_DEG = 150.0
#: The face points (nose, eyes, ears) count less: they are less sure, and
#: Anny's are not exactly where COCO's detector puts them.
FACE_WEIGHT = 0.3
HUBER_M = 0.05
#: The hip offsets may reach this far (metres).
MAX_HIP_OFFSET_M = 0.15
#: The weights of the smoothness of the rotations, hinge bends and position
#: (each per tick, against a data term that is a mean over the joints):
#: stronger, and the body lags behind 3D Pose's hips and legs.
SMOOTH_ROT, SMOOTH_BEND, SMOOTH_TRANS = 2.0, 2.0, 10.0
#: The pull towards an average body: weak, so a child keeps a child's shape.
SHAPE_PRIOR = 2e-5
SAMPLE_TICKS = 60
SHAPE_ITERS, POSE_ITERS = 600, 1500
CHUNK, OVERLAP = 600, 40
#: A tick the shape is fitted on has at least this many joints, hips and
#: shoulders among them; a person with none is not fitted.
MIN_JOINTS = 8


class AnnyFitter:
    """One Anny model on one device, fitting people one at a time."""

    def __init__(self, device: str | None = None, log=print):
        os.environ.setdefault("ANNY_CACHE_DIR", str(CACHE_DIR))
        import anny
        import torch
        from anny.keypoints import KeypointsRegressor

        self.torch = torch
        self.dev = device or ("cuda" if torch.cuda.is_available() else "cpu")
        if self.dev == "cpu":
            log("No GPU found: the avatar is fitted on the CPU, several times slower")
        self.model = anny.Anny().to(dtype=torch.float32, device=self.dev)
        self.reg = KeypointsRegressor.coco(self.model).to(self.dev)
        labels = list(self.model.bone_labels)
        self.labels = labels
        self.J = self.model.bone_count
        self.free = torch.tensor([labels.index(b) for b in FREE_BONES], device=self.dev)
        self.hinge = torch.tensor([labels.index(b) for b in HINGES], device=self.dev)
        self.root = labels.index("root")
        self.w = torch.ones(17, device=self.dev)
        self.w[:5] = FACE_WEIGHT

    # ── The body ───────────────────────────────────────────────────────────

    def _phenotype_kwargs(self, pheno, batch=1):
        return {
            k: pheno[i].reshape(1).expand(batch) for i, k in enumerate(self.model.phenotype_labels)
        }

    def rest(self, pheno):
        """The body at rest for a shape: its output (vertices, bone poses)."""
        torch = self.torch
        eye = torch.eye(4, device=self.dev)[None, None].repeat(1, self.J, 1, 1)
        return self.model(pose_parameters=eye, phenotype_kwargs=self._phenotype_kwargs(pheno))

    def keypoint_basis(self, pheno):
        """``(Q, rest_bone_poses)``: keypoint k = sum_j T_j @ Q[k, j] for
        this shape, T_j the bones' skinning transforms."""
        torch = self.torch
        rest = self.rest(pheno)
        rv = rest["rest_vertices"][0]
        rvh = torch.cat([rv, torch.ones_like(rv[:, :1])], 1)
        w = torch.zeros(rv.shape[0], self.J, device=self.dev)
        w.scatter_add_(
            1, self.model.vertex_bone_indices.long(), self.model.vertex_bone_weights.float()
        )
        if self.reg.is_sparse:
            r = torch.zeros(self.reg.regression_weights.shape[0], rv.shape[0], device=self.dev)
            r.scatter_add_(
                1, self.reg.regression_indices.long(), self.reg.regression_weights.float()
            )
        else:
            r = self.reg.regression_weights
        return torch.einsum("kv,vj,vx->kjx", r[:17], w, rvh), rest["rest_bone_poses"]

    def bone_transforms(self, pose, rest_bone_poses):
        import roma

        bt, _ = self.model.get_bone_transforms(pose, rest_bone_poses)
        if isinstance(bt, list):
            bt = roma.Rigid(
                self.torch.stack([t.linear for t in bt], 1),
                self.torch.stack([t.translation for t in bt], 1),
            ).to_homogeneous()
        return bt

    def pose_matrices(self, free_rv, bend, axes):
        import roma

        torch = self.torch
        pose = torch.eye(4, device=self.dev)[None, None].repeat(free_rv.shape[0], self.J, 1, 1)
        pose[:, self.free, :3, :3] = roma.rotvec_to_rotmat(free_rv)
        pose[:, self.hinge, :3, :3] = roma.rotvec_to_rotmat(bend[..., None] * axes[None])
        return pose

    def _kp_at(self, pose, Q, rest_bone_poses):
        bt = self.bone_transforms(pose, rest_bone_poses)
        return self.torch.einsum("bjxy,kjy->bkx", bt, Q)[..., :3]

    def hinges(self, Q, rest_bone_poses) -> dict:
        """Per hinge (in HINGES' order): the axis its bend turns about, and
        the bend's range from straight to MAX_BEND_DEG in all.

        Anny's pose parameters are changes from its reference pose, in which
        the elbows are already bent (about 36 degrees) and the knees nearly
        straight; so a bend is measured from there, and may be negative, down
        to straight. The axis is the one, among the bone frame's axes and the
        normal of the limb's own plane in the reference pose, whose rotation
        bends the limb most within that plane: the far end comes towards the
        near one and stays in the plane; knees backwards, elbows forwards
        (Anny faces -y) where the limb is straight and has no plane yet."""
        import roma

        torch = self.torch
        J = self.J
        eye = torch.eye(4, device=self.dev)[None, None].repeat(1, J, 1, 1)
        ref = self._kp_at(eye, Q, rest_bone_poses)[0]
        axes, lo, hi = [], [], []
        for bone, (far, mid, near) in HINGES.items():
            upper, fore = ref[mid] - ref[near], ref[far] - ref[mid]
            cos = torch.dot(upper, fore) / (torch.linalg.norm(upper) * torch.linalg.norm(fore))
            ref_bend = float(torch.arccos(cos.clamp(-1, 1)))
            normal = torch.linalg.cross(upper, fore)
            planar = float(torch.linalg.norm(normal)) > 1e-6 and ref_bend > np.deg2rad(10)
            frame_axes = ([1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1])
            cands = [torch.tensor(a, device=self.dev, dtype=torch.float32) for a in frame_axes]
            if planar:
                n = normal / torch.linalg.norm(normal)
                cands += [n, -n]
            best = None
            for a in cands:
                pose = eye.clone()
                pose[0, self.labels.index(bone), :3, :3] = roma.rotvec_to_rotmat(a * np.deg2rad(20))
                kp = self._kp_at(pose, Q, rest_bone_poses)[0]
                reach0 = torch.linalg.norm(ref[far] - ref[near])
                closer = float(reach0 - torch.linalg.norm(kp[far] - kp[near]))
                moved = kp[far] - ref[far]
                if planar:
                    off_plane = abs(float(torch.dot(moved, n)))
                    score = closer - 2.0 * off_plane
                else:
                    back = float(moved[1]) * (1 if "leg" in bone else -1)
                    score = closer + 0.5 * back
                if best is None or score > best[0]:
                    best = (score, a)
            axes.append(best[1])
            lo.append(-ref_bend)
            hi.append(np.deg2rad(MAX_BEND_DEG) - ref_bend)
        t = lambda v: torch.tensor(v, device=self.dev, dtype=torch.float32)  # noqa: E731
        return {"axes": torch.stack(axes), "lo": t(lo), "hi": t(hi)}

    def _bend(self, raw, hinges):
        return hinges["lo"] + self.torch.sigmoid(raw) * (hinges["hi"] - hinges["lo"])

    # ── The fit ────────────────────────────────────────────────────────────

    def _keypoints(self, p, Q, rbp, hinges, hip_offset):
        torch = self.torch
        bend = self._bend(p["bend"], hinges)
        bt = self.bone_transforms(self.pose_matrices(p["rot"], bend, hinges["axes"]), rbp)
        kp = torch.einsum("bjxy,kjy->bkx", bt, Q)[..., :3] + p["trans"][:, None]
        # COCO's hip points sit higher and wider than the hip joints: a
        # fixed offset per side, turning with the pelvis.
        root_r = bt[:, self.root, :3, :3]
        off = MAX_HIP_OFFSET_M * torch.tanh(hip_offset)
        kp = kp.clone()
        kp[:, L_HIP] = kp[:, L_HIP] + root_r @ off[0]
        kp[:, R_HIP] = kp[:, R_HIP] + root_r @ off[1]
        return kp, bt

    def _loss(self, kp, target, valid, p, smooth, pheno=None):
        torch = self.torch
        r2 = ((kp - target) ** 2).sum(-1)
        r = torch.sqrt(r2 + 1e-12)
        h = torch.where(r < HUBER_M, 0.5 * r2, HUBER_M * (r - 0.5 * HUBER_M))
        loss = (h * valid * self.w).sum() / valid.sum().clamp(min=1)
        loss = loss + 2e-4 * (p["rot"][:, 1:] ** 2).sum(-1).mean()
        if pheno is not None:
            loss = loss + SHAPE_PRIOR * (pheno**2).sum()  # towards an average body
        if smooth is not None and smooth.any():

            def d2(z):
                # Per tick: a bone's (or hinge's) squared second difference,
                # averaged over the bones; the position's summed over x, y, z.
                a = (z[2:] - 2 * z[1:-1] + z[:-2]) ** 2
                if a.ndim == 3:  # rotations: ticks x bones x 3
                    a = a.sum(-1).mean(-1)
                elif z is p["bend"]:  # ticks x hinges
                    a = a.mean(-1)
                else:  # the position: ticks x 3
                    a = a.sum(-1)
                return (a * smooth).sum() / smooth.sum()

            loss = (
                loss
                + SMOOTH_ROT * d2(p["rot"])
                + SMOOTH_BEND * d2(p["bend"])
                + SMOOTH_TRANS * d2(p["trans"])
            )
        return loss

    def _init(self, target, Q, rbp):
        """A start for ticks that have both hips: facing as the hips say,
        the body's hips on 3D Pose's, limbs nearly straight."""
        torch = self.torch
        n = target.shape[0]
        rot = torch.zeros(n, len(FREE_BONES), 3, device=self.dev)
        hips = (target[:, R_HIP] - target[:, L_HIP]).cpu().numpy()
        yaw = np.arctan2(hips[:, 1], hips[:, 0]) + np.pi
        rot[:, 0, 2] = torch.tensor(yaw, device=self.dev).float()
        eye = torch.eye(4, device=self.dev)[None, None].repeat(1, self.J, 1, 1)
        ref = self._kp_at(eye, Q, rbp)[0]
        trans = (target[:, L_HIP] + target[:, R_HIP]) / 2 - (ref[L_HIP] + ref[R_HIP]) / 2
        bend = torch.full((n, len(HINGES)), -2.0, device=self.dev)  # nearly straight
        return {"rot": rot, "bend": bend, "trans": trans}

    def fit(self, keypoints_zup: np.ndarray, ticks: np.ndarray, log=print) -> dict:
        """One person: ``keypoints_zup`` N x 17 x 3 (metres, z up, NaN where
        not found). Returns the shape, the hip offsets, every tick's pose
        parameters and fitted keypoints, and the hinge axes."""
        torch = self.torch
        valid_np = np.isfinite(keypoints_zup).all(-1)
        target = torch.tensor(np.nan_to_num(keypoints_zup), dtype=torch.float32, device=self.dev)
        valid = torch.tensor(valid_np, dtype=torch.float32, device=self.dev)
        n = len(ticks)
        good = np.flatnonzero(usable(valid_np))
        if len(good) == 0:
            raise ValueError(f"no tick with {MIN_JOINTS} joints, the hips and shoulders among them")
        # Stage 1: shape and hip offsets on a sample of ticks.
        sample = good[np.linspace(0, len(good) - 1, min(SAMPLE_TICKS, len(good))).astype(int)]
        sample = np.unique(sample)
        sel = torch.tensor(sample, device=self.dev)
        pheno = torch.zeros(len(self.model.phenotype_labels), device=self.dev, requires_grad=True)
        hip = torch.zeros(2, 3, device=self.dev, requires_grad=True)
        with torch.no_grad():
            Q, rbp = self.keypoint_basis(torch.sigmoid(pheno))
            hinges = self.hinges(Q, rbp)
            p = {k: v.requires_grad_(True) for k, v in self._init(target[sel], Q, rbp).items()}
        opt = torch.optim.Adam([*p.values(), pheno, hip], lr=0.03)
        for _ in range(SHAPE_ITERS):
            Q, rbp = self.keypoint_basis(torch.sigmoid(pheno))
            kp, _ = self._keypoints(p, Q, rbp, hinges, hip)
            loss = self._loss(kp, target[sel], valid[sel], p, None, pheno)
            loss = loss + 1e-4 * (hip**2).sum()
            opt.zero_grad()
            loss.backward()
            opt.step()
        pheno, hip = pheno.detach(), hip.detach()
        with torch.no_grad():
            Q, rbp = self.keypoint_basis(torch.sigmoid(pheno))
            hinges = self.hinges(Q, rbp)
        start = {k: v.detach() for k, v in p.items()}
        # Stage 2: every tick, in chunks, from the sampled tick nearest in time.
        out = {k: torch.zeros((n, *v.shape[1:]), device=self.dev) for k, v in start.items()}
        smooth_all = torch.tensor(smooth_mask(ticks), dtype=torch.float32, device=self.dev)
        nearest = torch.tensor(nearest_sample(ticks, sample), device=self.dev)
        sample_t = torch.tensor(sample, device=self.dev)
        hipmid = (target[:, L_HIP] + target[:, R_HIP]) / 2
        for c0, c1, k0, k1 in chunks(n, CHUNK, OVERLAP):
            idx = torch.arange(c0, c1, device=self.dev)
            near = nearest[c0:c1]
            q = {k: v[near].clone() for k, v in start.items()}
            seen = valid[idx, L_HIP] * valid[idx, R_HIP] * valid[sample_t[near], L_HIP]
            shift = (hipmid[idx] - hipmid[sample_t[near]]) * seen[:, None]
            q["trans"] = q["trans"] + shift
            q = {k: v.requires_grad_(True) for k, v in q.items()}
            opt = torch.optim.Adam(list(q.values()), lr=0.02)
            sm = smooth_all[c0 : max(c0, c1 - 2)]
            for _ in range(POSE_ITERS):
                kp, _ = self._keypoints(q, Q, rbp, hinges, hip)
                loss = self._loss(kp, target[idx], valid[idx], q, sm)
                opt.zero_grad()
                loss.backward()
                opt.step()
            for k in out:
                out[k][k0:k1] = q[k].detach()[k0 - c0 : k1 - c0]
            log(f"  {100.0 * k1 / n:5.1f}%  ({k1}/{n})")
        with torch.no_grad():  # in slices: one forward pass of a long recording is big
            kp = torch.cat(
                [
                    self._keypoints(
                        {k: v[s : s + 1000] for k, v in out.items()}, Q, rbp, hinges, hip
                    )[0]
                    for s in range(0, n, 1000)
                ]
            )
        err = torch.linalg.norm(kp - target, dim=-1).cpu().numpy()
        return {
            "phenotype": dict(
                zip(self.model.phenotype_labels, torch.sigmoid(pheno).tolist(), strict=True)
            ),
            "pheno": pheno,
            "hip_offset_m": (MAX_HIP_OFFSET_M * torch.tanh(hip)).cpu().numpy(),
            "params": out,
            "hinges": hinges,
            "keypoints": kp.cpu().numpy(),
            "error_m": np.where(valid_np, err, np.nan),
        }

    # ── What the app draws ─────────────────────────────────────────────────

    def export(self, fit: dict) -> dict:
        """The arrays of the avatar file (avatar.io): the body at rest, its
        skinning, and every tick's bone transforms as quaternions and
        translations (the fit's frame, metres)."""
        import roma

        torch = self.torch
        with torch.no_grad():
            rest = self.rest(torch.sigmoid(fit["pheno"]))
            _Q, rbp = self.keypoint_basis(torch.sigmoid(fit["pheno"]))
            p = fit["params"]
            bend = self._bend(p["bend"], fit["hinges"])
            bones = []
            for s in range(0, p["rot"].shape[0], 1000):
                bt = self.bone_transforms(
                    self.pose_matrices(
                        p["rot"][s : s + 1000], bend[s : s + 1000], fit["hinges"]["axes"]
                    ),
                    rbp,
                )
                q = roma.rotmat_to_unitquat(bt[..., :3, :3])  # x, y, z, w
                t = bt[..., :3, 3] + p["trans"][s : s + 1000, None]
                bones.append(torch.cat([q, t], -1).cpu())
        return {
            "rest_vertices": rest["rest_vertices"][0].cpu().numpy(),
            "faces": np.asarray(self.model.faces, dtype=np.uint32),
            "skin_bones": self.model.vertex_bone_indices.cpu().numpy().astype(np.uint16),
            "skin_weights": self.model.vertex_bone_weights.cpu().numpy().astype(np.float32),
            "bones": torch.cat(bones).numpy(),
        }
