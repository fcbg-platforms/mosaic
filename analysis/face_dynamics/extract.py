"""
Per-frame face measurements for Face Dynamics: eye aspect ratios, the 52
MediaPipe blendshapes and head yaw/pitch/roll.

MediaPipe FaceLandmarker's own detector is made for a face filling the frame;
a face across a room (70 to 90 pixels wide) it mostly misses. So, as in the 3D
gaze plugin (:mod:`gaze.face_detect`), YuNet finds the face and FaceLandmarker
measures it on a square crop scaled to :data:`CROP_PX`. The crop then follows
the landmarks from frame to frame. Interview mode (a large face at about
50 fps) gives the best measurements; a room camera works with coarser blinks.

Head pose reuses the 3D gaze plugin's metric canonical face and PnP
(:mod:`gaze.canonical`, :mod:`gaze.head_pose`). With the camera's intrinsic
calibration (from ``session_meta.json``) it is metric; without it a nominal
focal length is used, which changes the angles only slightly.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

#: Six eyelid landmarks per eye for the eye aspect ratio, in the order
#: outer corner, upper lid (2), inner corner, lower lid (2). "Right" is the
#: subject's own right eye (on the image left).
RIGHT_EAR_IDS = (33, 160, 158, 133, 153, 144)
LEFT_EAR_IDS = (263, 387, 385, 362, 380, 373)
#: Landmarks drawn on the annotated video: eye contours and the outer lips.
EYE_OUTLINE_RIGHT = (33, 246, 161, 160, 159, 158, 157, 173, 133, 155, 154, 153, 145, 144, 163, 7)
EYE_OUTLINE_LEFT = (263, 466, 388, 387, 386, 385, 384, 398, 362, 382, 381, 380, 374, 373, 390, 249)
NOSE_TIP = 1
#: FaceLandmarker runs on a square crop of this many pixels: the face fills it
#: whether it was 80 pixels wide (a room camera) or 400 (interview mode).
CROP_PX = 256
#: Crop side relative to a YuNet box, and to the previous frame's landmarks
#: (which reach from brow to chin, so are larger than a YuNet box).
DETECT_CROP = 1.6
TRACK_CROP = 1.4
LIPS_OUTER = (
    61,
    185,
    40,
    39,
    37,
    0,
    267,
    269,
    270,
    409,
    291,
    375,
    321,
    405,
    314,
    17,
    84,
    181,
    91,
    146,
)


@dataclass
class FrameFace:
    """What one frame shows of the face (angles are ``nan`` when the pose
    fit failed)."""

    ear_right: float
    ear_left: float
    yaw: float  # degrees, positive towards the subject's left (image right)
    pitch: float  # degrees, positive up
    roll: float  # degrees, positive when the head tilts clockwise in the image
    blendshapes: dict
    box: tuple  # (x1, y1, x2, y2), px
    outline: np.ndarray  # (N, 2) px: right eye, left eye, lips
    nose: tuple  # nose tip (x, y), px
    #: All 478 landmarks (px) and the head pose (:class:`gaze.head_pose.HeadPose`,
    #: camera frame, or ``None``), for gaze (:mod:`eye_contact`).
    landmarks: np.ndarray | None = None
    pose: object | None = None


def head_angles(r_cam_head) -> tuple[float, float, float]:
    """Yaw, pitch and roll of a head relative to the camera, degrees.

    The head frame looks along -z (see :mod:`gaze.ray_math`); facing the
    camera squarely is ``(0, 0, 0)``. Yaw is positive when the face turns
    towards image right (the subject's left), pitch positive when it looks
    up, roll positive when the head tilts clockwise as seen in the image.
    """
    r = np.asarray(r_cam_head, dtype=np.float64).reshape(3, 3)
    fwd = r @ np.array([0.0, 0.0, -1.0])  # where the face points, camera frame
    side = r @ np.array([1.0, 0.0, 0.0])  # towards the subject's left
    # The camera looks along +z, so a face looking back at it points along -z.
    yaw = math.degrees(math.atan2(fwd[0], -fwd[2]))
    pitch = math.degrees(math.atan2(-fwd[1], math.hypot(fwd[0], fwd[2])))
    roll = math.degrees(math.atan2(side[1], side[0]))
    return yaw, pitch, roll


def nominal_camera(width: int, height: int) -> tuple[np.ndarray, np.ndarray]:
    """A stand-in intrinsic matrix (focal length = image width) when the
    camera is not calibrated."""
    f = float(max(width, height))
    k = np.array([[f, 0.0, width / 2.0], [0.0, f, height / 2.0], [0.0, 0.0, 1.0]])
    return k, np.zeros(5)


class FaceTracker:
    """One face per video: YuNet finds it, FaceLandmarker (blendshapes on)
    measures it on a crop, and head pose comes from PnP.

    The crop follows the face: each frame is cut around the previous frame's
    landmarks, and YuNet is run again only when the face is lost. When a frame
    holds several faces, the largest is taken.

    Parameters
    ----------
    camera_matrix, dist_coeffs : array_like
        The camera's calibration (or :func:`nominal_camera`).
    min_confidence : float
        Face detection and presence threshold.
    """

    def __init__(self, camera_matrix, dist_coeffs, min_confidence: float = 0.5) -> None:
        import cv2
        import mediapipe as mp
        from gaze.canonical import load_canonical_face
        from gaze.face_detect import landmarker_path, yunet_path
        from mediapipe.tasks import python as mp_python
        from mediapipe.tasks.python import vision as mp_vision

        task = landmarker_path()
        canonical = load_canonical_face(task)
        self._rigid = np.asarray(canonical.rigid_ids)
        self._model = canonical.points(self._rigid)
        self._k = np.asarray(camera_matrix, dtype=np.float64).reshape(3, 3)
        self._d = np.asarray(dist_coeffs, dtype=np.float64).reshape(-1)
        self._cv2 = cv2
        self._mp = mp
        self._detector = cv2.FaceDetectorYN_create(
            str(yunet_path()), "", (320, 320), score_threshold=min_confidence
        )
        self._det_size = None
        options = mp_vision.FaceLandmarkerOptions(
            base_options=mp_python.BaseOptions(model_asset_path=str(task)),
            running_mode=mp_vision.RunningMode.IMAGE,
            num_faces=1,
            min_face_detection_confidence=min_confidence,
            min_face_presence_confidence=min_confidence,
            output_face_blendshapes=True,
            output_facial_transformation_matrixes=False,
        )
        self._landmarker = mp_vision.FaceLandmarker.create_from_options(options)
        self._last_box = None  # landmark extent in the previous frame

    def close(self) -> None:
        self._landmarker.close()

    def _detect(self, frame_bgr: np.ndarray):
        h, w = frame_bgr.shape[:2]
        if self._det_size != (w, h):
            self._detector.setInputSize((w, h))
            self._det_size = (w, h)
        _, dets = self._detector.detect(frame_bgr)
        if dets is None or len(dets) == 0:
            return None
        x, y, bw, bh = (float(v) for v in max(dets, key=lambda d: d[2] * d[3])[:4])
        return (x, y, x + bw, y + bh)

    def _landmarks(self, frame_bgr: np.ndarray, box, factor: float):
        """Landmarks (478, 2) px and blendshapes on a crop around ``box``."""
        from gaze.face_detect import crop_to_frame, cut_crop, square_crop

        cv2 = self._cv2
        x0, y0, side = square_crop(box, factor)
        crop = cut_crop(frame_bgr, x0, y0, side)
        interp = cv2.INTER_AREA if side > CROP_PX else cv2.INTER_CUBIC
        crop = cv2.resize(crop, (CROP_PX, CROP_PX), interpolation=interp)
        rgb = cv2.cvtColor(crop, cv2.COLOR_BGR2RGB)
        result = self._landmarker.detect(
            self._mp.Image(image_format=self._mp.ImageFormat.SRGB, data=rgb)
        )
        if not result.face_landmarks:
            return None
        lm = result.face_landmarks[0]
        px = crop_to_frame([[p.x, p.y] for p in lm], x0, y0, side)
        shapes = {}
        if result.face_blendshapes:
            shapes = {c.category_name: float(c.score) for c in result.face_blendshapes[0]}
        return px, shapes

    def measure(self, frame_bgr: np.ndarray) -> FrameFace | None:
        """The face in one frame, or ``None``."""
        from gaze.head_pose import solve_pnp

        from .metrics import eye_aspect_ratio

        found = None
        if self._last_box is not None:
            found = self._landmarks(frame_bgr, self._last_box, TRACK_CROP)
        if found is None:
            box = self._detect(frame_bgr)
            if box is not None:
                found = self._landmarks(frame_bgr, box, DETECT_CROP)
        if found is None:
            self._last_box = None
            return None
        px, shapes = found
        box = (*px.min(axis=0), *px.max(axis=0))
        self._last_box = box

        yaw = pitch = roll = float("nan")
        pose = solve_pnp(self._model, px[self._rigid], self._k, self._d)
        if pose is not None:
            yaw, pitch, roll = head_angles(pose.r)
        return FrameFace(
            eye_aspect_ratio(px[list(RIGHT_EAR_IDS)]),
            eye_aspect_ratio(px[list(LEFT_EAR_IDS)]),
            yaw,
            pitch,
            roll,
            shapes,
            tuple(float(v) for v in box),
            px[list(EYE_OUTLINE_RIGHT) + list(EYE_OUTLINE_LEFT) + list(LIPS_OUTER)],
            (float(px[NOSE_TIP, 0]), float(px[NOSE_TIP, 1])),
            px,
            pose,
        )
