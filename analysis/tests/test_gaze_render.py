"""
Gaze Fusion's renderers (gaze/render.py) on a small synthetic room: two
cameras and two subjects looking at each other. Checks what is drawn where,
and that the videos and heat maps come out complete. OpenCV and numpy only.
"""

import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).parent.parent))

from gaze import render
from gaze.render import RenderData, SubjectTrack

W, H = 320, 240
FPS = 10.0
N = 20
K = np.array([[300.0, 0.0, W / 2], [0.0, 300.0, H / 2], [0.0, 0.0, 1.0]])


def translation(x, y, z):
    m = np.eye(4)
    m[:3, 3] = [x, y, z]
    return m


def make_data() -> RenderData:
    # Two subjects 2 m in front of camera 0, 600 mm apart, looking at each
    # other; camera 1 sits 500 mm to the right of camera 0.
    a, b = np.array([-300.0, 0.0, 2000.0]), np.array([300.0, 0.0, 2000.0])
    ticks = np.arange(N)
    s1 = SubjectTrack(
        name="S1",
        ticks=ticks,
        origins=np.tile(a, (N, 1)),
        points=np.tile(b, (N, 1)),
        labels=["S1 <-> S2"] * N,
        mutual=np.ones(N, bool),
        boxes={0: {int(t): np.array([90.0, 100.0, 120.0, 140.0]) for t in ticks}},
    )
    # S2 has no gaze for its first half (nan points), and stops after tick 14.
    pts = np.tile(a, (15, 1))
    pts[:10] = np.nan
    s2 = SubjectTrack(
        name="S2 (interviewer)",
        ticks=ticks[:15],
        origins=np.tile(b, (15, 1)),
        points=pts,
        labels=["S2"] * 15,
        mutual=np.zeros(15, bool),
    )
    cams = {0: np.eye(4), 1: translation(-500.0, 0.0, 0.0)}
    return RenderData(
        tracks=[s1, s2],
        times_s=np.arange(N) / FPS,
        fps=FPS,
        cameras={i: (m, K, np.zeros(5)) for i, m in cams.items()},
        room_from_camera={i: np.linalg.inv(m) for i, m in cams.items()},
        regions=[
            (
                "screen",
                np.array(
                    [[-400, -300, 2500], [400, -300, 2500], [400, 300, 2500], [-400, 300, 2500]],
                    float,
                ),
            )
        ],
        plane=None,
        hold_ticks=2,
        trail_ticks=5,
    )


def test_track_holds_an_estimate_for_a_few_ticks_only():
    tr = make_data().tracks[1]  # S2: ticks 0..14
    assert tr.at(14, 2) == 14
    assert tr.at(16, 2) == 14  # held
    assert tr.at(17, 2) is None  # too old
    assert tr.at(-1, 2) is None


def test_points_behind_or_beside_the_camera_are_not_projected():
    cam = make_data().cameras[0]
    px = render._project(cam, [[0, 0, 2000], [0, 0, -2000], [5000, 0, 1000]])
    assert np.allclose(px[0], [W / 2, H / 2])
    assert np.isnan(px[1]).all()  # behind
    assert np.isnan(px[2]).all()  # far outside the field of view


def test_overlay_draws_the_gaze_point_and_the_box():
    data = make_data()
    frame = np.zeros((H, W, 3), np.uint8)
    render.draw_camera_overlay(frame, 0, data.cameras[0], data, tick=5)
    # S1's gaze point is S2's origin: 300 * 300 / 2000 + 160 = 205 px.
    u, v = 205, 120
    assert frame[v - 2 : v + 3, u - 2 : u + 3].any()
    # S1's face box, in its colour.
    assert tuple(frame[125, 90]) == render.PALETTE[0]  # left edge, below the label
    # Nothing drawn far from the subjects.
    assert not frame[220:, :40].any()


def test_camera_video_has_one_frame_per_tick_and_no_partial_file(tmp_path):
    src = tmp_path / "video_0.mp4"
    out = cv2.VideoWriter(str(src), cv2.VideoWriter_fourcc(*"mp4v"), FPS, (W, H))
    for i in range(N):
        out.write(np.full((H, W, 3), 40 + i, np.uint8))
    out.release()
    frame_index = np.arange(N)
    frame_index[7] = -1  # a missing frame: the previous one is shown again
    dest = tmp_path / "gaze_fusion" / "video_0.gaze.mp4"
    dest.parent.mkdir()

    cam, err = render.render_camera_video((0, str(src), frame_index, str(dest), make_data()))

    assert (cam, err) == (0, None)
    assert dest.exists() and not render.partial_path(dest).exists()
    cap = cv2.VideoCapture(str(dest))
    n = 0
    while cap.read()[0]:
        n += 1
    assert n == N


def test_camera_video_reports_a_missing_source(tmp_path):
    cam, err = render.render_camera_video(
        (3, str(tmp_path / "nope.mp4"), np.arange(3), str(tmp_path / "o.mp4"), make_data())
    )
    assert cam == 3 and "cannot open" in err


def test_topdown_video_and_heatmaps(tmp_path):
    data = make_data()
    out = tmp_path / "room_topdown.mp4"
    render.render_topdown(out, data)
    cap = cv2.VideoCapture(str(out))
    assert (
        int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)),
        int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT)),
    ) == render.TOPDOWN_SIZE
    n = 0
    while cap.read()[0]:
        n += 1
    assert n == N

    paths = render.write_heatmaps(tmp_path, data)
    assert [p.name for p in paths] == ["heatmap_S1.png", "heatmap_S2__interviewer_.png"]
    for p in paths:
        img = cv2.imread(str(p))
        assert img.shape[1::-1] == render.TOPDOWN_SIZE
