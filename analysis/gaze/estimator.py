"""
Per-camera 3D gaze-ray estimation backend for the post-hoc Multi-Camera
Gaze Fusion plugin (analysis/run_gaze_fusion.py).

Detects a face with MediaPipe FaceLandmarker using its OWN
FaceLandmarkerOptions instance (output_face_blendshapes=False,
output_facial_transformation_matrixes=False — this module solves head pose
itself via cv2.solvePnP against the camera's REAL calibrated intrinsics,
rather than trusting MediaPipe's own facial_transformation_matrix, which is
generated against a synthetic weak-perspective camera model and isn't
metrically comparable to this app's calibrated cameras). Not importing
python/pose/gaze_estimator.py or analysis/expression/detector.py directly —
same cross-project/cross-plugin boundary rule already documented in
analysis/expression/detector.py's module docstring; the relevant landmark
indices and iris-offset heuristic are duplicated here instead.

Only the OpenCV/MediaPipe-dependent pieces (detection, cv2.solvePnP) live
here. The pure linear algebra that turns (rotation, translation, gaze_dx,
gaze_dy) into an actual 3D ray, and later fuses multiple cameras' rays,
lives in ray_math.py with zero cv2/mediapipe imports, so that module stays
unit-testable without either heavy dependency installed.
"""
from __future__ import annotations

import urllib.request
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

_MODELS_DIR = Path(__file__).parent / "models"

_MEDIAPIPE_MODEL_URL = (
    "https://storage.googleapis.com/mediapipe-models/"
    "face_landmarker/face_landmarker/float16/1/face_landmarker.task"
)

# ── Landmark indices ─────────────────────────────────────────────────────
# Same MediaPipe FaceLandmarker 478-point mesh python/pose/gaze_estimator.py
# uses for its 2D-only heuristic; duplicated rather than imported (see
# module docstring).
_LEFT_IRIS_CENTER, _RIGHT_IRIS_CENTER = 468, 473
_LEFT_EYE_OUTER, _LEFT_EYE_INNER      = 33, 133
_LEFT_EYE_TOP, _LEFT_EYE_BOTTOM       = 159, 145
_RIGHT_EYE_INNER, _RIGHT_EYE_OUTER    = 362, 263
_RIGHT_EYE_TOP, _RIGHT_EYE_BOTTOM     = 386, 374

_NOSE_TIP, _CHIN = 1, 152
_LEFT_MOUTH, _RIGHT_MOUTH = 61, 291

# 6-point head-pose landmark set, index-aligned with _HEAD_POSE_MODEL_MM
# below — the standard correspondence set used by most OpenCV
# head-pose-estimation references.
_HEAD_POSE_LANDMARK_IDS = [
    _NOSE_TIP, _CHIN, _LEFT_EYE_OUTER, _RIGHT_EYE_OUTER, _LEFT_MOUTH, _RIGHT_MOUTH,
]

# Generic 3D face model, millimetres, nose-tip-relative. Not
# subject-specific (real face geometry varies), so the resulting head pose
# is an approximation — acceptable here since gaze fusion only needs an
# eye-region ray origin + forward direction, not millimetre head-shape
# accuracy.
HEAD_POSE_MODEL_MM = np.array([
    [0.0, 0.0, 0.0],          # nose tip
    [0.0, -330.0, -65.0],     # chin
    [-225.0, 170.0, -135.0],  # left eye outer corner
    [225.0, 170.0, -135.0],   # right eye outer corner
    [-150.0, -150.0, -125.0], # left mouth corner
    [150.0, -150.0, -125.0],  # right mouth corner
], dtype=np.float64)

# Midpoint between the two eye-outer-corner model points — used as the
# gaze ray's metric origin (a stand-in for "between the eyes").
EYE_ORIGIN_MODEL_MM = (HEAD_POSE_MODEL_MM[2] + HEAD_POSE_MODEL_MM[3]) / 2.0


@dataclass
class FaceGazeSample:
    """One detected face's head pose and 2D iris-offset heuristic.

    Attributes
    ----------
    bbox_xyxy : tuple of float
        Detection bounding box, ``(x1, y1, x2, y2)`` pixels.
    confidence : float
        Detection confidence. Always ``1.0`` (FaceLandmarkerResult
        carries no per-face detection score).
    head_rotation : numpy.ndarray
        3x3 head-pose rotation, camera-local (from ``cv2.solvePnP``).
    head_translation : numpy.ndarray
        Length-3 head-pose translation, camera-local, mm.
    gaze_dx, gaze_dy : float
        2D iris-offset heuristic, each in ``[-1, 1]`` — same heuristic as
        ``python/pose/gaze_estimator.py``.
    """
    bbox_xyxy: tuple[float, float, float, float]   # pixel coords
    confidence: float
    head_rotation: np.ndarray      # 3x3, camera-local
    head_translation: np.ndarray   # length-3, camera-local, mm
    gaze_dx: float                 # -1..1, same heuristic as python/pose/gaze_estimator.py
    gaze_dy: float


class MediaPipeGazeEstimator3D:
    """MediaPipe Tasks ``FaceLandmarker`` + ``cv2.solvePnP``-based 3D gaze estimator.

    Thread-unsafe — one instance per process, matching every other
    MediaPipe-backed detector in this codebase.

    Parameters
    ----------
    max_faces : int, default 1
        Maximum simultaneous faces to detect per frame. The Multi-Camera
        Gaze Fusion pipeline only ever consumes subject 0, so this is
        left at its single-subject default in practice.
    min_confidence : float, default 0.5
        Minimum face-detection/-presence confidence to keep a face.
    """

    def __init__(self, max_faces: int = 1, min_confidence: float = 0.5) -> None:
        import mediapipe as mp
        from mediapipe.tasks import python as mp_python
        from mediapipe.tasks.python import vision as mp_vision

        model_path = _ensure_download(_MODELS_DIR / "face_landmarker.task", _MEDIAPIPE_MODEL_URL)

        options = mp_vision.FaceLandmarkerOptions(
            base_options=mp_python.BaseOptions(model_asset_path=str(model_path)),
            running_mode=mp_vision.RunningMode.IMAGE,
            num_faces=max_faces,
            min_face_detection_confidence=min_confidence,
            min_face_presence_confidence=min_confidence,
            output_face_blendshapes=False,
            output_facial_transformation_matrixes=False,
        )
        self._mp = mp
        self._landmarker = mp_vision.FaceLandmarker.create_from_options(options)

    def detect(self, frame_bgr: np.ndarray,
               camera_matrix: np.ndarray, dist_coeffs: np.ndarray) -> list[FaceGazeSample]:
        """Detect faces and solve each one's 3D head pose in one BGR frame.

        Parameters
        ----------
        frame_bgr : numpy.ndarray
            BGR frame, as returned by ``cv2.imread``/``cv2.VideoCapture``.
        camera_matrix : numpy.ndarray
            3x3 camera intrinsic matrix — this camera's real calibration
            (read from ``session_meta.json`` by the caller), required for
            a metric ``solvePnP`` solve.
        dist_coeffs : numpy.ndarray
            Length-5 distortion coefficients, this camera's real
            calibration.

        Returns
        -------
        list of FaceGazeSample
            One entry per successfully-solved face. Faces missing iris
            landmarks, with a degenerate eye width, or a failed
            ``solvePnP`` fit are silently skipped — a documented,
            low-risk degrade (one fewer camera contributing to that
            frame's fusion), not a crash.
        """
        h, w = frame_bgr.shape[:2]
        rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        mp_image = self._mp.Image(image_format=self._mp.ImageFormat.SRGB, data=rgb)
        result = self._landmarker.detect(mp_image)

        camera_matrix = np.asarray(camera_matrix, dtype=np.float64)
        dist_coeffs   = np.asarray(dist_coeffs, dtype=np.float64)

        samples: list[FaceGazeSample] = []
        for lm in result.face_landmarks:
            if len(lm) < 478:
                continue   # iris landmarks not present in this model output

            xs = [p.x for p in lm[:468]]
            ys = [p.y for p in lm[:468]]
            bbox = (min(xs) * w, min(ys) * h, max(xs) * w, max(ys) * h)

            image_points = np.array(
                [[lm[i].x * w, lm[i].y * h] for i in _HEAD_POSE_LANDMARK_IDS],
                dtype=np.float64)

            ok, rvec, tvec = cv2.solvePnP(
                HEAD_POSE_MODEL_MM, image_points, camera_matrix, dist_coeffs,
                flags=cv2.SOLVEPNP_ITERATIVE)
            if not ok:
                continue

            rotation, _ = cv2.Rodrigues(rvec)

            gaze_dx, gaze_dy = _iris_offset(lm)
            if gaze_dx is None:
                continue

            samples.append(FaceGazeSample(
                bbox_xyxy=bbox,
                confidence=1.0,
                head_rotation=rotation,
                head_translation=tvec.reshape(3),
                gaze_dx=gaze_dx,
                gaze_dy=gaze_dy,
            ))
        return samples


def _iris_offset(lm):
    """Compute the 2D iris-centre-offset-from-eye-socket-centre heuristic.

    Parameters
    ----------
    lm : sequence
        MediaPipe FaceLandmarker's 478-point face-mesh landmark list for
        one detected face.

    Returns
    -------
    tuple of (float, float) or tuple of (None, None)
        ``(gaze_dx, gaze_dy)``, each in ``[-1, 1]``, or ``(None, None)``
        if the eye width is too small to normalise by.

    Notes
    -----
    Same heuristic as ``python/pose/gaze_estimator.py``'s
    ``GazeEstimator.estimate()`` — iris-centre offset from eye-socket
    centre, normalised by eye width.
    """
    li_x, li_y = lm[_LEFT_IRIS_CENTER].x, lm[_LEFT_IRIS_CENTER].y
    ri_x, ri_y = lm[_RIGHT_IRIS_CENTER].x, lm[_RIGHT_IRIS_CENTER].y

    left_cx = (lm[_LEFT_EYE_OUTER].x + lm[_LEFT_EYE_INNER].x) / 2
    left_cy = (lm[_LEFT_EYE_TOP].y + lm[_LEFT_EYE_BOTTOM].y) / 2
    left_w  = abs(lm[_LEFT_EYE_OUTER].x - lm[_LEFT_EYE_INNER].x)

    right_cx = (lm[_RIGHT_EYE_INNER].x + lm[_RIGHT_EYE_OUTER].x) / 2
    right_cy = (lm[_RIGHT_EYE_TOP].y + lm[_RIGHT_EYE_BOTTOM].y) / 2
    right_w  = abs(lm[_RIGHT_EYE_INNER].x - lm[_RIGHT_EYE_OUTER].x)

    eye_w = (left_w + right_w) / 2
    if eye_w < 1e-6:
        return None, None

    dx = ((li_x - left_cx) / eye_w + (ri_x - right_cx) / eye_w) / 2
    dy = ((li_y - left_cy) / eye_w + (ri_y - right_cy) / eye_w) / 2
    return float(max(-1.0, min(1.0, dx))), float(max(-1.0, min(1.0, dy)))


def _ensure_download(dest: Path, url: str) -> Path:
    """Download ``url`` to ``dest`` if not already cached there.

    Parameters
    ----------
    dest : pathlib.Path
        Destination file path; parent directories are created as needed.
    url : str
        Source URL.

    Returns
    -------
    pathlib.Path
        ``dest``, unchanged.
    """
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        print(f"[gaze] Downloading {dest.name} …", flush=True)
        urllib.request.urlretrieve(url, dest)
        print(f"[gaze] Downloaded to {dest}", flush=True)
    return dest
