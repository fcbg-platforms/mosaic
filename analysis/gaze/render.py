"""
Annotated videos and heat maps of where every subject looks.

* ``gaze_fusion/video_N.gaze.mp4``, one per camera, frame ``k`` = tick ``k``:
  each subject's face box and name, the gaze ray projected into that
  camera, the gaze point, a fading trail of the last
  :data:`TRAIL_SECONDS`, and what the subject looks at (``S1 -> S2``,
  ``S1 -> screen``, ``S1 <-> S2`` for mutual gaze).
* ``gaze_fusion/room_topdown.mp4``: the room from above, with cameras,
  regions, subjects, gaze rays and points, over a heat layer that builds up
  as the recording plays.
* ``gaze_fusion/heatmap_<subject>.png``: where each subject's gaze landed,
  seen from above.

Between analysed ticks (``--skip``) the last estimate is held for up to
:data:`HOLD_SECONDS`. Each camera renders in its own process; the inputs
are plain arrays (:class:`RenderData`) so they pickle cheaply.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from .head_pose import project_points
from .ray_math import unit

TRAIL_SECONDS = 1.5
HOLD_SECONDS = 0.3
#: BGR colours per subject (S1, S2, ...), chosen to stay distinct on video.
PALETTE = [
    (60, 200, 255),
    (255, 160, 40),
    (90, 230, 90),
    (230, 90, 230),
    (255, 255, 90),
    (80, 120, 255),
]
TOPDOWN_SIZE = (1100, 800)


@dataclass
class SubjectTrack:
    """One subject's analysed estimates, in tick order.

    Attributes
    ----------
    name : str
        Display name.
    ticks : numpy.ndarray
        ``(n,)`` analysed ticks.
    origins, points : numpy.ndarray
        ``(n, 3)`` gaze origin and gaze point (``nan`` rows: no gaze).
    labels : list of str
        Text drawn for each estimate.
    mutual : numpy.ndarray
        ``(n,)`` bool.
    boxes : dict
        ``{camera: {tick: box}}`` where that camera saw the face.
    """

    name: str
    ticks: np.ndarray
    origins: np.ndarray
    points: np.ndarray
    labels: list
    mutual: np.ndarray
    boxes: dict = field(default_factory=dict)

    def at(self, tick: int, hold_ticks: int) -> int | None:
        """Index of the estimate to show at ``tick`` (the latest one at or
        before it, if not older than ``hold_ticks``)."""
        i = int(np.searchsorted(self.ticks, tick, side="right")) - 1
        if i < 0 or tick - int(self.ticks[i]) > hold_ticks:
            return None
        return i


@dataclass
class RenderData:
    """Everything the renderers need, as plain picklable data."""

    tracks: list
    times_s: np.ndarray
    fps: float
    cameras: dict  # {index: (camera_from_room (4,4), K (3,3), dist (n,))}
    room_from_camera: dict  # {index: (4,4)}
    regions: list  # [(name, corners (4,3))]
    plane: tuple | None
    hold_ticks: int = 0
    trail_ticks: int = 0


def build_render_data(
    entries, subjects, names, timeline, cameras, regions, plane, skip
) -> RenderData:
    """Collect the finished entries into :class:`RenderData`."""
    from .io_v2 import display

    tracks = []
    for s in subjects:
        mine = sorted((e for e in entries if e.subject == s), key=lambda e: e.tick)
        if not mine:
            continue
        origins = np.array([e.origin for e in mine])
        points = np.array(
            [
                e.target.point if e.target is not None and e.direction is not None else [np.nan] * 3
                for e in mine
            ]
        )
        labels, mutual = [], []
        for e in mine:
            who = display(s, names)
            if e.target is None or e.direction is None:
                labels.append(who)
            elif e.mutual:
                labels.append(f"{who} <-> {display(e.label, names)}")
            elif e.label_kind == "subject":
                labels.append(f"{who} -> {display(e.label, names)}")
            elif e.label and e.label != "none":
                labels.append(f"{who} -> {e.label}")
            else:
                labels.append(who)
            mutual.append(bool(e.mutual))
        boxes: dict = {}
        for e in mine:
            for c in e.per_camera:
                boxes.setdefault(c.camera, {})[e.tick] = np.asarray(c.box, dtype=np.float64)
        tracks.append(
            SubjectTrack(
                name=display(s, names),
                ticks=np.array([e.tick for e in mine]),
                origins=origins,
                points=points,
                labels=labels,
                mutual=np.array(mutual),
                boxes=boxes,
            )
        )
    times_s = (np.asarray(timeline.times_ns, dtype=np.float64) - float(timeline.times_ns[0])) / 1e9
    return RenderData(
        tracks=tracks,
        times_s=times_s,
        fps=timeline.fps,
        cameras={
            i: (c.camera_from_room, c.camera_matrix, c.dist_coeffs) for i, c in cameras.items()
        },
        room_from_camera={i: c.extrinsic_rt for i, c in cameras.items()},
        regions=[(r.name, r.corners()) for r in regions],
        plane=plane,
        hold_ticks=max(skip, int(round(HOLD_SECONDS * timeline.fps))),
        trail_ticks=int(round(TRAIL_SECONDS * timeline.fps)),
    )


def open_writer(path: Path, fps: float, size: tuple[int, int]):
    """An mp4 writer, H.264 when this OpenCV build has it, else MPEG-4."""
    import cv2

    for code in ("avc1", "mp4v"):
        w = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*code), fps, size)
        if w.isOpened():
            return w
        w.release()
    return None


def partial_path(path: Path) -> Path:
    """``name.partial.mp4``: written first, renamed when complete, so a
    crashed render never leaves a file that looks finished."""
    return path.with_name(path.stem + ".partial" + path.suffix)


# ── Camera videos ────────────────────────────────────────────────────────────


#: Points further off a camera's axis than this (normalised image
#: coordinate, |x/z| or |y/z|) are not drawn: beyond the calibrated field of
#: view the distortion polynomial can fold them back into the image.
MAX_NORMALISED = 1.3


def _project(cam, pts) -> np.ndarray:
    """Pixels of room points in one camera; ``nan`` for points behind it or
    outside its calibrated field of view."""
    camera_from_room, k, dist = cam
    p = np.asarray(pts, dtype=np.float64).reshape(-1, 3)
    pc = p @ camera_from_room[:3, :3].T + camera_from_room[:3, 3]
    px = project_points(pc, k, dist)
    with np.errstate(divide="ignore", invalid="ignore"):
        off = np.maximum(np.abs(pc[:, 0] / pc[:, 2]), np.abs(pc[:, 1] / pc[:, 2]))
    px[~(pc[:, 2] > 0) | ~(off <= MAX_NORMALISED)] = np.nan
    return px


def _text(img, text, org, scale, color):
    import cv2

    thick = max(1, int(round(scale * 2)))
    (tw, th), base = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, scale, thick)
    x, y = int(org[0]), int(org[1])
    cv2.rectangle(img, (x - 4, y - th - 6), (x + tw + 4, y + base + 2), (20, 20, 20), -1)
    cv2.putText(img, text, (x, y), cv2.FONT_HERSHEY_SIMPLEX, scale, color, thick, cv2.LINE_AA)


def draw_camera_overlay(frame, cam_index: int, cam, data: RenderData, tick: int) -> None:
    """Draw every subject's gaze for one tick onto one camera's frame.

    The fading trails are blended in first, half transparent; boxes, rays,
    gaze points and labels are then drawn on top at full strength.
    """
    import cv2

    h, w = frame.shape[:2]
    s = h / 1080.0
    overlay = frame.copy()
    any_trail = False
    shown = []
    for k, tr in enumerate(data.tracks):
        color = PALETTE[k % len(PALETTE)]
        i = tr.at(tick, data.hold_ticks)
        # Trail: earlier gaze points, oldest smallest.
        lo = int(np.searchsorted(tr.ticks, tick - data.trail_ticks, side="left"))
        hi = i + 1 if i is not None else int(np.searchsorted(tr.ticks, tick, side="right"))
        trail = [j for j in range(lo, hi) if np.isfinite(tr.points[j]).all()]
        if trail:
            px = _project(cam, tr.points[trail])
            for j, (u, v) in zip(trail, px, strict=True):
                if np.isfinite(u) and 0 <= u < w and 0 <= v < h:
                    age = (tick - tr.ticks[j]) / max(data.trail_ticks, 1)
                    radius = max(2, int((6 - 4 * age) * s))
                    cv2.circle(overlay, (int(u), int(v)), radius, color, -1, cv2.LINE_AA)
                    any_trail = True
        if i is not None:
            shown.append((tr, i, color))
    if any_trail:
        cv2.addWeighted(overlay, 0.55, frame, 0.45, 0, dst=frame)

    thick = max(1, int(2 * s))
    for tr, i, color in shown:
        origin, point = tr.origins[i], tr.points[i]
        box = tr.boxes.get(cam_index, {}).get(int(tr.ticks[i]))
        if box is not None:
            cv2.rectangle(
                frame, (int(box[0]), int(box[1])), (int(box[2]), int(box[3])), color, thick
            )
            anchor = (box[0], box[1] - 8 * s)
        else:
            o_px = _project(cam, origin)[0]
            if not (np.isfinite(o_px).all() and 0 <= o_px[0] < w and 0 <= o_px[1] < h):
                continue  # this subject is not in this camera's view
            anchor = (o_px[0] + 10 * s, o_px[1] - 10 * s)
        if np.isfinite(point).all():
            line = origin[None, :] + np.linspace(0.0, 1.0, 32)[:, None] * (point - origin)[None, :]
            px = _project(cam, line)
            # Draw each visible stretch of the ray separately.
            run: list = []
            for u, v in list(px) + [(np.nan, np.nan)]:
                if np.isfinite(u):
                    run.append((int(u), int(v)))
                    continue
                if len(run) >= 2:
                    cv2.polylines(
                        frame, [np.array(run, dtype=np.int32)], False, color, thick, cv2.LINE_AA
                    )
                run = []
            p_px = _project(cam, point)[0]
            if np.isfinite(p_px).all() and 0 <= p_px[0] < w and 0 <= p_px[1] < h:
                c = (int(p_px[0]), int(p_px[1]))
                cv2.circle(frame, c, int(9 * s), (255, 255, 255), -1, cv2.LINE_AA)
                cv2.circle(frame, c, int(6 * s), color, -1, cv2.LINE_AA)
        _text(frame, tr.labels[i], anchor, 0.7 * s, color)


def render_camera_video(args) -> tuple[int, str | None]:
    """Worker: write one camera's annotated video.

    Parameters
    ----------
    args : tuple
        ``(cam_index, video_path, frame_index, out_path, data)`` with
        ``frame_index`` the source frame per tick (-1 for none).

    Returns
    -------
    tuple
        ``(cam_index, error or None)``.
    """
    import cv2

    cam_index, video_path, frame_index, out_path, data = args
    out_path = Path(out_path)
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        return cam_index, f"cannot open {video_path}"
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    tmp = partial_path(out_path)
    writer = open_writer(tmp, data.fps, (w, h))
    if writer is None:
        cap.release()
        return cam_index, f"cannot open a video writer for {tmp}"
    cam = data.cameras.get(cam_index)
    pos = -1
    frame = np.zeros((h, w, 3), dtype=np.uint8)
    for tick, want in enumerate(np.asarray(frame_index)):
        if want >= 0:
            # Frames are wanted in non-decreasing order: skip forward by
            # grabbing, never seek (seeking is slow and inexact).
            while pos < want:
                ok = cap.grab()
                if not ok:
                    break
                pos += 1
            if pos == want:
                ok, img = cap.retrieve()
                if ok:
                    frame = img
        out = frame.copy()
        if cam is not None:
            draw_camera_overlay(out, cam_index, cam, data, tick)
        writer.write(out)
    cap.release()
    writer.release()
    tmp.replace(out_path)
    return cam_index, None


# ── Top-down view ────────────────────────────────────────────────────────────


class TopDown:
    """The room seen from above, in a fixed pixel canvas.

    "Up" is the calibrated plane's normal when there is one (pointing to the
    side the cameras are on), else the reference camera's up (-y). The
    canvas covers the cameras, regions, subjects and the central 96% of gaze
    points, plus a margin.
    """

    def __init__(self, data: RenderData) -> None:
        cams = np.array([m[:3, 3] for m in data.room_from_camera.values()])
        if data.plane is not None:
            up = unit(data.plane[1])
            if len(cams) and np.dot(cams.mean(axis=0) - data.plane[0], up) < 0:
                up = -up
        else:
            up = np.array([0.0, -1.0, 0.0])
        ref_x = np.array([1.0, 0.0, 0.0])
        e1 = unit(ref_x - np.dot(ref_x, up) * up)
        if np.linalg.norm(e1) < 0.5:
            e1 = unit(np.cross(up, [0.0, 0.0, 1.0]))
        # e1 to the right and e2 drawn downwards must look *down* the up
        # axis (e1 x e2 = -up); cross(up, e1) would show the room from below,
        # mirrored.
        self.e1, self.e2 = e1, np.cross(e1, up)

        pts = [cams] if len(cams) else []
        for tr in data.tracks:
            pts.append(tr.origins)
            good = tr.points[np.isfinite(tr.points).all(axis=1)]
            if len(good):
                lo, hi = np.percentile(good, [2, 98], axis=0)
                pts.append(good[((good >= lo) & (good <= hi)).all(axis=1)])
        for _name, corners in data.regions:
            pts.append(corners)
        allp = np.vstack(pts) if pts else np.zeros((1, 3))
        uv = self.to_plane(allp)
        lo, hi = uv.min(axis=0) - 400.0, uv.max(axis=0) + 400.0
        w, h = TOPDOWN_SIZE
        legend = 260
        self.scale = min(
            (w - legend - 40) / max(hi[0] - lo[0], 1.0), (h - 80) / max(hi[1] - lo[1], 1.0)
        )
        self.lo = lo
        self.size = (w, h)
        self.offset = np.array([20.0, 60.0])

    def to_plane(self, p) -> np.ndarray:
        p = np.asarray(p, dtype=np.float64).reshape(-1, 3)
        return np.column_stack([p @ self.e1, p @ self.e2])

    def px(self, p) -> np.ndarray:
        uv = self.to_plane(p)
        return (uv - self.lo) * self.scale + self.offset

    def base(self, data: RenderData):
        """The static picture: grid, cameras, regions."""
        import cv2

        w, h = self.size
        img = np.full((h, w, 3), 24, dtype=np.uint8)
        step = 500.0 * self.scale
        if step > 8:
            for x in np.arange(self.offset[0], w - 260, step):
                cv2.line(img, (int(x), 50), (int(x), h - 10), (40, 40, 40), 1)
            for y in np.arange(self.offset[1], h - 10, step):
                cv2.line(img, (10, int(y)), (w - 270, int(y)), (40, 40, 40), 1)
        for name, corners in data.regions:
            poly = self.px(corners).astype(np.int32)
            cv2.polylines(img, [poly], True, (200, 200, 200), 2, cv2.LINE_AA)
            c = poly.mean(axis=0)
            cv2.putText(
                img,
                name,
                (int(c[0]) - 20, int(c[1])),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.5,
                (220, 220, 220),
                1,
                cv2.LINE_AA,
            )
        for i, m in sorted(data.room_from_camera.items()):
            pos = self.px(m[:3, 3])[0]
            fwd = self.px(m[:3, 3] + 400.0 * m[:3, 2])[0]
            cv2.arrowedLine(
                img,
                tuple(int(v) for v in pos),
                tuple(int(v) for v in fwd),
                (150, 150, 150),
                2,
                tipLength=0.3,
            )
            cv2.circle(img, tuple(int(v) for v in pos), 6, (210, 210, 210), -1)
            cv2.putText(
                img,
                f"Camera {i + 1}",
                (int(pos[0]) + 8, int(pos[1]) - 8),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.45,
                (210, 210, 210),
                1,
                cv2.LINE_AA,
            )
        cv2.putText(
            img,
            "Room from above (0.5 m grid)",
            (20, 30),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.7,
            (230, 230, 230),
            2,
            cv2.LINE_AA,
        )
        return img

    def splat(self, heat: np.ndarray, point, sigma_px: float = 6.0) -> None:
        """Add one gaze point to a heat map (a small Gaussian)."""
        u, v = self.px(point)[0]
        if not np.isfinite(u):
            return
        r = int(3 * sigma_px)
        x0, y0 = int(u) - r, int(v) - r
        ys, xs = np.mgrid[0 : 2 * r + 1, 0 : 2 * r + 1]
        g = np.exp(-((xs - r) ** 2 + (ys - r) ** 2) / (2 * sigma_px**2))
        h, w = heat.shape
        xa, ya = max(x0, 0), max(y0, 0)
        xb, yb = min(x0 + 2 * r + 1, w), min(y0 + 2 * r + 1, h)
        if xb > xa and yb > ya:
            heat[ya:yb, xa:xb] += g[ya - y0 : yb - y0, xa - x0 : xb - x0]


def _heat_layer(base, heat, scale: float | None = None):
    """Blend a heat map over ``base``. ``scale`` is the heat value shown at
    full strength (default: the map's maximum); the colours are compressed
    with a square root so a few long fixations do not hide everything else."""
    import cv2

    top = float(heat.max()) if scale is None else scale
    if top <= 0:
        return base.copy()
    norm = np.clip(heat / top, 0, 1)
    colour = cv2.applyColorMap((norm * 255).astype(np.uint8), cv2.COLORMAP_INFERNO)
    alpha = (np.sqrt(norm) * 0.85)[..., None]
    return (base * (1 - alpha) + colour * alpha).astype(np.uint8)


def render_topdown(out_path: Path, data: RenderData) -> None:
    """Write the top-down room video."""
    import cv2

    view = TopDown(data)
    base = view.base(data)
    heat = np.zeros(base.shape[:2], dtype=np.float64)
    tmp = partial_path(out_path)
    writer = open_writer(tmp, data.fps, view.size)
    if writer is None:
        raise RuntimeError(f"cannot open a video writer for {tmp}")
    w = view.size[0]
    for tick in range(len(data.times_s)):
        for tr in data.tracks:
            j = int(np.searchsorted(tr.ticks, tick))
            if j < len(tr.ticks) and tr.ticks[j] == tick and np.isfinite(tr.points[j]).all():
                view.splat(heat, tr.points[j])
        frame = _heat_layer(base, heat)
        y = 60
        for k, tr in enumerate(data.tracks):
            color = PALETTE[k % len(PALETTE)]
            i = tr.at(tick, data.hold_ticks)
            if i is None:
                continue
            o = view.px(tr.origins[i])[0]
            cv2.circle(frame, (int(o[0]), int(o[1])), 12, color, 2, cv2.LINE_AA)
            cv2.putText(
                frame,
                tr.name,
                (int(o[0]) + 14, int(o[1]) + 5),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.55,
                color,
                2,
                cv2.LINE_AA,
            )
            if np.isfinite(tr.points[i]).all():
                p = view.px(tr.points[i])[0]
                cv2.line(
                    frame, (int(o[0]), int(o[1])), (int(p[0]), int(p[1])), color, 2, cv2.LINE_AA
                )
                cv2.circle(frame, (int(p[0]), int(p[1])), 6, color, -1, cv2.LINE_AA)
            cv2.putText(
                frame,
                tr.labels[i],
                (w - 250, y),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.55,
                color,
                2,
                cv2.LINE_AA,
            )
            y += 28
        t = data.times_s[tick]
        cv2.putText(
            frame,
            f"{int(t // 60):02d}:{t % 60:05.2f}",
            (w - 250, 30),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.7,
            (230, 230, 230),
            2,
            cv2.LINE_AA,
        )
        writer.write(frame)
    writer.release()
    tmp.replace(out_path)


def write_heatmaps(out_dir: Path, data: RenderData) -> list[Path]:
    """One top-down heat map PNG per subject."""
    import cv2

    view = TopDown(data)
    base = view.base(data)
    out = []
    for k, tr in enumerate(data.tracks):
        heat = np.zeros(base.shape[:2], dtype=np.float64)
        for p in tr.points:
            if np.isfinite(p).all():
                view.splat(heat, p)
        img = _heat_layer(base, heat)
        med = np.median(tr.origins, axis=0)
        o = view.px(med)[0]
        color = PALETTE[k % len(PALETTE)]
        cv2.circle(img, (int(o[0]), int(o[1])), 12, color, 2, cv2.LINE_AA)
        cv2.putText(
            img,
            f"{tr.name}: where the gaze landed",
            (20, 30),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.7,
            color,
            2,
            cv2.LINE_AA,
        )
        path = out_dir / f"heatmap_{_safe(tr.name)}.png"
        cv2.imwrite(str(path), img)
        out.append(path)
    return out


def _safe(name: str) -> str:
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in name) or "subject"
