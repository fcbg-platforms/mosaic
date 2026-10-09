"""
Faces and landmarks at room distance.

MediaPipe FaceLandmarker carries its own face detector, a short-range model
made for faces filling a phone camera's frame. Across a room a face is only
70 to 90 pixels wide in a 1080p frame and it mostly finds nothing. So the two
jobs are split:

1. **YuNet** (OpenCV's ``FaceDetectorYN``), built for faces at any size,
   finds every face in the full frame.
2. Each face is cut out as a square crop :data:`CROP_FACTOR` times its size,
   padded where it leaves the frame, and FaceLandmarker runs on that crop,
   where the face is large again. Its normalised landmarks map back to
   full-frame pixels with :func:`crop_to_frame`.

Only the landmarks the gaze pipeline uses are kept (:func:`landmark_ids`),
about 60 of the 478, so a per-camera cache of a long session stays small.
"""

from __future__ import annotations

import hashlib
import urllib.request
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .canonical import (
    FALLBACK_RIGID_IDS,
    LEFT_EYE_BOTTOM,
    LEFT_EYE_INNER,
    LEFT_EYE_OUTER,
    LEFT_EYE_TOP,
    LEFT_IRIS,
    MIDLINE_IDS,
    RIGHT_EYE_BOTTOM,
    RIGHT_EYE_INNER,
    RIGHT_EYE_OUTER,
    RIGHT_EYE_TOP,
    RIGHT_IRIS,
)

MODELS_DIR = Path(__file__).parent / "models"

LANDMARKER_URL = (
    "https://storage.googleapis.com/mediapipe-models/"
    "face_landmarker/face_landmarker/float16/1/face_landmarker.task"
)
# media.githubusercontent.com, not raw.githubusercontent.com: the raw URL
# serves a Git LFS pointer instead of the model.
YUNET_URL = (
    "https://media.githubusercontent.com/media/opencv/opencv_zoo/main/"
    "models/face_detection_yunet/face_detection_yunet_2023mar.onnx"
)
YUNET_SHA256 = "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4"

#: Crop side relative to the larger side of the detected face box.
CROP_FACTOR = 1.6
#: Crops smaller than this are upscaled before landmarking.
MIN_CROP_PX = 192


def landmark_ids(rigid_ids) -> np.ndarray:
    """Sorted landmark ids the pipeline needs: the rigid head-pose set, eye
    corners and lids, both irises (centre and rim), and the midline."""
    eye = (
        RIGHT_EYE_OUTER,
        RIGHT_EYE_INNER,
        RIGHT_EYE_TOP,
        RIGHT_EYE_BOTTOM,
        LEFT_EYE_OUTER,
        LEFT_EYE_INNER,
        LEFT_EYE_TOP,
        LEFT_EYE_BOTTOM,
    )
    ids = set(rigid_ids) | set(FALLBACK_RIGID_IDS) | set(eye) | set(RIGHT_IRIS)
    ids |= set(LEFT_IRIS) | set(MIDLINE_IDS)
    return np.asarray(sorted(ids), dtype=np.int64)


def square_crop(box, factor: float = CROP_FACTOR):
    """Square crop around a face box. It may extend past the frame; it is
    padded when cut (:func:`cut_crop`), so the face stays centred.

    Parameters
    ----------
    box : array_like
        ``(x1, y1, x2, y2)`` in pixels.
    factor : float
        Crop side relative to the box's larger side.

    Returns
    -------
    tuple
        ``(x0, y0, side)``: top-left corner (may be negative) and side, px.
    """
    x1, y1, x2, y2 = (float(v) for v in box)
    side = max(x2 - x1, y2 - y1) * factor
    cx, cy = (x1 + x2) / 2.0, (y1 + y2) / 2.0
    return int(round(cx - side / 2.0)), int(round(cy - side / 2.0)), max(int(round(side)), 1)


def cut_crop(frame: np.ndarray, x0: int, y0: int, side: int) -> np.ndarray:
    """The square region, zero-padded where it leaves the frame."""
    h, w = frame.shape[:2]
    out = np.zeros((side, side) + frame.shape[2:], dtype=frame.dtype)
    sx0, sy0 = max(x0, 0), max(y0, 0)
    sx1, sy1 = min(x0 + side, w), min(y0 + side, h)
    if sx1 > sx0 and sy1 > sy0:
        out[sy0 - y0 : sy1 - y0, sx0 - x0 : sx1 - x0] = frame[sy0:sy1, sx0:sx1]
    return out


def crop_to_frame(norm_xy, x0: int, y0: int, side: int) -> np.ndarray:
    """Map landmarks normalised to a square crop back to frame pixels.

    MediaPipe normalises x by the image width and y by its height; on a
    square crop both are ``side``, however the crop was resized.
    """
    p = np.asarray(norm_xy, dtype=np.float64).reshape(-1, 2)
    return np.column_stack([x0 + p[:, 0] * side, y0 + p[:, 1] * side])


@dataclass
class FaceLandmarks:
    """One face in one frame.

    Attributes
    ----------
    box : numpy.ndarray
        YuNet box ``(x1, y1, x2, y2)``, px.
    score : float
        YuNet confidence.
    points : numpy.ndarray
        ``(len(ids), 2)`` landmark pixels, in the order of the ``ids``
        passed to :class:`FaceFinder`.
    """

    box: np.ndarray
    score: float
    points: np.ndarray


def _download(dest: Path, url: str, sha256: str | None = None) -> Path:
    def digest(p: Path) -> str:
        h = hashlib.sha256()
        with p.open("rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()

    if dest.exists() and (sha256 is None or digest(dest) == sha256):
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"[gaze] Downloading {dest.name} ...", flush=True)
    partial = dest.with_suffix(dest.suffix + ".part")
    urllib.request.urlretrieve(url, partial)
    if sha256 is not None and digest(partial) != sha256:
        partial.unlink(missing_ok=True)
        raise RuntimeError(f"{dest.name}: downloaded file does not match its pinned sha256")
    partial.replace(dest)
    return dest


def landmarker_path() -> Path:
    """The FaceLandmarker bundle, downloaded on first use."""
    return _download(MODELS_DIR / "face_landmarker.task", LANDMARKER_URL)


def yunet_path() -> Path:
    """The YuNet face detector, downloaded on first use."""
    return _download(MODELS_DIR / "face_detection_yunet_2023mar.onnx", YUNET_URL, YUNET_SHA256)


class FaceFinder:
    """YuNet + FaceLandmarker on crops (see the module docstring).

    Parameters
    ----------
    ids : array_like
        Landmark ids to keep, from :func:`landmark_ids`.
    min_score : float
        YuNet confidence threshold.
    max_faces : int
        At most this many faces per frame, largest first.
    """

    def __init__(self, ids, min_score: float = 0.6, max_faces: int = 6) -> None:
        import cv2
        import mediapipe as mp
        from mediapipe.tasks import python as mp_python
        from mediapipe.tasks.python import vision as mp_vision

        self._cv2 = cv2
        self._mp = mp
        self._ids = np.asarray(ids, dtype=np.int64)
        self._max_faces = max_faces
        self._detector = cv2.FaceDetectorYN_create(
            str(yunet_path()), "", (320, 320), score_threshold=min_score
        )
        self._size = None
        options = mp_vision.FaceLandmarkerOptions(
            base_options=mp_python.BaseOptions(model_asset_path=str(landmarker_path())),
            running_mode=mp_vision.RunningMode.IMAGE,
            num_faces=1,
            min_face_detection_confidence=0.3,
            min_face_presence_confidence=0.3,
            output_face_blendshapes=False,
            output_facial_transformation_matrixes=False,
        )
        self._landmarker = mp_vision.FaceLandmarker.create_from_options(options)

    def close(self) -> None:
        self._landmarker.close()

    def find(self, frame_bgr: np.ndarray) -> list[FaceLandmarks]:
        """Every face in a BGR frame, with landmarks."""
        cv2 = self._cv2
        h, w = frame_bgr.shape[:2]
        if self._size != (w, h):
            self._detector.setInputSize((w, h))
            self._size = (w, h)
        _, dets = self._detector.detect(frame_bgr)
        if dets is None:
            return []
        dets = sorted(dets, key=lambda d: -(d[2] * d[3]))[: self._max_faces]

        faces: list[FaceLandmarks] = []
        for det in dets:
            x, y, bw, bh = (float(v) for v in det[:4])
            box = np.array([x, y, x + bw, y + bh])
            x0, y0, side = square_crop(box)
            crop = cut_crop(frame_bgr, x0, y0, side)
            if side < MIN_CROP_PX:
                crop = cv2.resize(crop, (MIN_CROP_PX, MIN_CROP_PX), interpolation=cv2.INTER_CUBIC)
            rgb = cv2.cvtColor(crop, cv2.COLOR_BGR2RGB)
            result = self._landmarker.detect(
                self._mp.Image(image_format=self._mp.ImageFormat.SRGB, data=rgb)
            )
            if not result.face_landmarks:
                continue
            lm = result.face_landmarks[0]
            if len(lm) <= int(self._ids.max()):
                continue  # no iris landmarks in this model output
            norm = np.array([[lm[i].x, lm[i].y] for i in self._ids])
            points = crop_to_frame(norm, x0, y0, side)
            faces.append(FaceLandmarks(box=box, score=float(det[-1]), points=points))
        return _dedupe(faces)


def _dedupe(faces: list[FaceLandmarks]) -> list[FaceLandmarks]:
    """Drop a face whose landmarks land on another, higher-scoring face (two
    overlapping boxes can make the landmarker find the same face twice)."""
    kept: list[FaceLandmarks] = []
    for f in sorted(faces, key=lambda f: -f.score):
        c = f.points.mean(axis=0)
        size = max(f.box[2] - f.box[0], f.box[3] - f.box[1])
        if all(np.linalg.norm(c - k.points.mean(axis=0)) > 0.3 * size for k in kept):
            kept.append(f)
    return kept
