"""
Face-detection backends for the anonymization plugin.  All three expose the
same interface — ``detect(frame_bgr) -> list[(x1, y1, x2, y2)]`` in pixel
coordinates — so run_face_mask.py doesn't need to know which one is in use.
Each backend already filters by its own confidence threshold internally
before returning, so the box tuples don't carry a confidence score.

Backends (selectable in the UI, "mediapipe" is the default):

  mediapipe — MediaPipe Tasks FaceLandmarker, same model family already used
              by python/pose/gaze_estimator.py for real-time gaze (but that
              module lives in a different, independent uv project — python/
              vs. analysis/ — so this backend owns its own model download
              rather than importing across the project boundary). Best
              recall; multi-face.
  yolov8    — Ultralytics YOLO with a face-detection checkpoint. There is no
              Ultralytics-official face weights file (unlike yolov8n-pose.pt),
              so this downloads a named community checkpoint on first use.
              Pass --model /path/to/local.pt to supply your own instead.
  opencv    — cv2.dnn res10 SSD Caffe face detector (official OpenCV-hosted
              model/prototxt). No new ML framework dependency, weakest recall
              of the three on extreme angles.

Each backend caches its downloaded model file(s) under models/ next to this
file (gitignored, same treatment as auto-downloaded yolov8n-pose.pt).
"""
from __future__ import annotations

import urllib.request
from pathlib import Path
from typing import Optional, Protocol

import cv2
import numpy as np

Box = tuple[float, float, float, float]  # x1, y1, x2, y2

_MODELS_DIR = Path(__file__).parent / "models"


class FaceDetector(Protocol):
    """Structural interface every face-detection backend satisfies.

    All three backends (:class:`MediaPipeFaceDetector`,
    :class:`YoloFaceDetector`, :class:`OpenCVDnnFaceDetector`) implement
    this without inheriting from it — a plain ``duck-typing`` Protocol so
    ``run_face_mask.py`` doesn't need to know which one is in use.
    """

    def detect(self, frame_bgr: np.ndarray) -> list[Box]:
        """Detect faces in one BGR frame.

        Parameters
        ----------
        frame_bgr : numpy.ndarray
            BGR frame, as returned by ``cv2.imread``/``cv2.VideoCapture``.

        Returns
        -------
        list of Box
            One ``(x1, y1, x2, y2)`` pixel box per detected face, already
            filtered by this backend's own confidence threshold (box
            tuples carry no separate confidence score).
        """
        ...


# ── MediaPipe FaceLandmarker ─────────────────────────────────────────────────

_MEDIAPIPE_MODEL_URL = (
    "https://storage.googleapis.com/mediapipe-models/"
    "face_landmarker/face_landmarker/float16/1/face_landmarker.task"
)


class MediaPipeFaceDetector:
    """MediaPipe Tasks ``FaceLandmarker``-based face detector.

    Best recall of the three backends, supports multiple faces. Downloads
    ``face_landmarker.task`` to ``models/`` on first use.

    Parameters
    ----------
    conf_threshold : float, default 0.5
        Minimum face-detection/-presence confidence to keep a face.
    max_faces : int, default 10
        Maximum simultaneous faces to detect per frame.
    """

    def __init__(self, conf_threshold: float = 0.5, max_faces: int = 10) -> None:
        import mediapipe as mp
        from mediapipe.tasks import python as mp_python
        from mediapipe.tasks.python import vision as mp_vision

        model_path = _ensure_download(
            _MODELS_DIR / "face_landmarker.task", _MEDIAPIPE_MODEL_URL)

        options = mp_vision.FaceLandmarkerOptions(
            base_options=mp_python.BaseOptions(model_asset_path=str(model_path)),
            running_mode=mp_vision.RunningMode.IMAGE,
            num_faces=max_faces,
            min_face_detection_confidence=conf_threshold,
            min_face_presence_confidence=conf_threshold,
            output_face_blendshapes=False,
            output_facial_transformation_matrixes=False,
        )
        self._mp = mp
        self._landmarker = mp_vision.FaceLandmarker.create_from_options(options)

    def detect(self, frame_bgr: np.ndarray) -> list[Box]:
        """See :meth:`FaceDetector.detect`."""
        h, w = frame_bgr.shape[:2]
        rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        mp_image = self._mp.Image(image_format=self._mp.ImageFormat.SRGB, data=rgb)
        result = self._landmarker.detect(mp_image)

        boxes: list[Box] = []
        for face_landmarks in result.face_landmarks:
            xs = [lm.x for lm in face_landmarks]
            ys = [lm.y for lm in face_landmarks]
            boxes.append((min(xs) * w, min(ys) * h, max(xs) * w, max(ys) * h))
        return boxes


# ── YOLOv8-face (ultralytics) ────────────────────────────────────────────────

# Community-maintained checkpoints (not hosted/affiliated with Ultralytics) —
# see https://github.com/akanametov/yolo-face. Only used when --model isn't
# given a local path explicitly.
_YOLO_FACE_URLS = {
    "yolov8n-face.pt": "https://github.com/akanametov/yolo-face/releases/download/v0.0.0/yolov8n-face.pt",
    "yolov8m-face.pt": "https://github.com/akanametov/yolo-face/releases/download/v0.0.0/yolov8m-face.pt",
}


class YoloFaceDetector:
    """Ultralytics YOLO face detector, using a community face checkpoint.

    There is no Ultralytics-official face-detection weights file (unlike
    ``yolov8n-pose.pt``), so this downloads a named community checkpoint
    on first use (see the module docstring) unless ``model`` points at a
    local file.

    Parameters
    ----------
    model : str or None, default None
        A local checkpoint path, a known community checkpoint name (see
        ``_YOLO_FACE_URLS``), or ``None`` for the default
        ``"yolov8n-face.pt"``.
    conf_threshold : float, default 0.5
        Minimum detection confidence to keep a face.
    device : str or None, default None
        Inference device (e.g. ``"cpu"``, ``"cuda:0"``); ``None`` lets
        ultralytics choose.
    """

    def __init__(self, model: Optional[str] = None, conf_threshold: float = 0.5,
                 device: Optional[str] = None) -> None:
        from ultralytics import YOLO

        model_name = model or "yolov8n-face.pt"
        model_path: str
        if Path(model_name).is_file():
            model_path = model_name
        elif model_name in _YOLO_FACE_URLS:
            model_path = str(_ensure_download(
                _MODELS_DIR / model_name, _YOLO_FACE_URLS[model_name]))
        else:
            # Let ultralytics try its own resolution (e.g. an official name).
            model_path = model_name

        self._conf = conf_threshold
        self._device = device
        self._model = YOLO(model_path)

    def detect(self, frame_bgr: np.ndarray) -> list[Box]:
        """See :meth:`FaceDetector.detect`."""
        results = self._model(frame_bgr, conf=self._conf, device=self._device, verbose=False)
        boxes: list[Box] = []
        for res in results:
            if res.boxes is None:
                continue
            for b in res.boxes.xyxy.cpu().numpy():
                boxes.append((float(b[0]), float(b[1]), float(b[2]), float(b[3])))
        return boxes


# ── OpenCV DNN (res10 SSD) ───────────────────────────────────────────────────

_OPENCV_PROTOTXT_URL = (
    "https://raw.githubusercontent.com/opencv/opencv/master/samples/dnn/face_detector/deploy.prototxt"
)
_OPENCV_MODEL_URL = (
    "https://raw.githubusercontent.com/opencv/opencv_3rdparty/"
    "dnn_samples_face_detector_20170830/res10_300x300_ssd_iter_140000.caffemodel"
)


class OpenCVDnnFaceDetector:
    """``cv2.dnn`` res10 SSD Caffe face detector.

    No new ML framework dependency beyond ``opencv-python``, but the
    weakest recall of the three backends on extreme head angles.
    Downloads the official OpenCV-hosted prototxt/caffemodel to
    ``models/`` on first use.

    Parameters
    ----------
    conf_threshold : float, default 0.5
        Minimum detection confidence to keep a face.
    """

    def __init__(self, conf_threshold: float = 0.5) -> None:
        prototxt = _ensure_download(_MODELS_DIR / "deploy.prototxt", _OPENCV_PROTOTXT_URL)
        caffemodel = _ensure_download(
            _MODELS_DIR / "res10_300x300_ssd_iter_140000.caffemodel", _OPENCV_MODEL_URL)
        self._net = cv2.dnn.readNetFromCaffe(str(prototxt), str(caffemodel))
        self._conf = conf_threshold

    def detect(self, frame_bgr: np.ndarray) -> list[Box]:
        """See :meth:`FaceDetector.detect`."""
        h, w = frame_bgr.shape[:2]
        # blobFromImage already resizes to the given size — no need to
        # cv2.resize() first too.
        blob = cv2.dnn.blobFromImage(frame_bgr, 1.0, (300, 300), (104.0, 177.0, 123.0))
        self._net.setInput(blob)
        detections = self._net.forward()

        boxes: list[Box] = []
        for i in range(detections.shape[2]):
            conf = float(detections[0, 0, i, 2])
            if conf < self._conf:
                continue
            box = detections[0, 0, i, 3:7] * np.array([w, h, w, h])
            x1, y1, x2, y2 = box.tolist()
            boxes.append((x1, y1, x2, y2))
        return boxes


# ── Shared model-download helper ─────────────────────────────────────────────

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
        ``dest``, unchanged — returned for convenient call-site chaining.
    """
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        print(f"[facemask] Downloading {dest.name} …", flush=True)
        urllib.request.urlretrieve(url, dest)
        print(f"[facemask] Downloaded to {dest}", flush=True)
    return dest


# ── Factory ───────────────────────────────────────────────────────────────────

def make_detector(backend: str, model: Optional[str], conf_threshold: float,
                   device: Optional[str] = None) -> FaceDetector:
    """Build a face-detection backend by name.

    Parameters
    ----------
    backend : {"mediapipe", "yolov8", "opencv"}
        Which backend to construct.
    model : str or None
        Forwarded to :class:`YoloFaceDetector` as its ``model`` argument;
        ignored by the other two backends.
    conf_threshold : float
        Minimum detection confidence to keep a face.
    device : str or None, default None
        Forwarded to :class:`YoloFaceDetector` as its ``device`` argument;
        ignored by the other two backends.

    Returns
    -------
    FaceDetector
        A constructed, ready-to-use detector.

    Raises
    ------
    ValueError
        If ``backend`` isn't one of the three known names.
    """
    if backend == "mediapipe":
        return MediaPipeFaceDetector(conf_threshold=conf_threshold)
    if backend == "yolov8":
        return YoloFaceDetector(model=model, conf_threshold=conf_threshold, device=device)
    if backend == "opencv":
        return OpenCVDnnFaceDetector(conf_threshold=conf_threshold)
    raise ValueError(f"Unknown face-detection backend: {backend!r}")
