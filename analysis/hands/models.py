"""The two hand models, behind one call: ``estimator(rgb, people)`` gives the
hands in one frame, each ``{"subject_id", "side", "score", "keypoints"
(21 x [x, y] px), "scores" (21)}``.

* ``rtmw`` (the default): RTMW, a whole-body model (133 points), run on each
  person's Pose box, gives each hand with the person's own left and right;
  the RTMPose hand model then refines each hand on a close crop. Both are
  ONNX files run by onnxruntime. The crop and decoding follow rtmlib
  (https://github.com/Tau-J/rtmlib, Apache-2.0); the models are
  OpenMMLab's (MMPose, Apache-2.0).
* ``mediapipe``: MediaPipe's Hand Landmarker on a crop around each wrist
  Pose found; person and side from the nearest wrist.

Both need the camera's Pose result: it says where the people are.
"""

from __future__ import annotations

import zipfile
from pathlib import Path

import cv2
import numpy as np

from hands.people import assign_by_wrist, distinct_people, drop_repeats, hand_seeds, hand_size

MODELS_DIR = Path(__file__).parent / "models"
RTMW_URL = (
    "https://download.openmmlab.com/mmpose/v1/projects/rtmw/onnx_sdk/"
    "rtmw-dw-x-l_simcc-cocktail14_270e-384x288_20231122.zip"
)
RTMW_SHA256 = "a87e1af41a0a067776dba7d46e1c21c8f6e9f18e247e0e606718dd1f31e96ffd"
HAND_URL = (
    "https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/onnx_sdk/"
    "rtmpose-m_simcc-hand5_pt-aic-coco_210e-256x256-74fb594_20230320.zip"
)
HAND_SHA256 = "45c9e1aa21b8fe33859973c49bce4647645d312e7a226e398a42f9137d44a56d"
MEDIAPIPE_URL = (
    "https://storage.googleapis.com/mediapipe-models/hand_landmarker/"
    "hand_landmarker/float16/1/hand_landmarker.task"
)
MEDIAPIPE_SHA256 = "fbc2a30080c3c557093b5ddfc334698132eb341044ccee322ccf8bcf3607cde1"

#: RTMW's points for each hand (COCO-WholeBody): the person's own sides.
RTMW_HANDS = {"left": 91, "right": 112}
#: A hand whose points score below this on average is not kept.
MIN_SCORE = 0.3
MEAN = np.array([123.675, 116.28, 103.53])
STD = np.array([58.395, 57.12, 57.375])


def _onnx_from_zip(name: str, url: str, sha256: str) -> Path:
    """The model's ``end2end.onnx``, kept as ``models/<name>.onnx``; the
    downloaded zip is checked against its pinned sha256, then removed."""
    from gaze.face_detect import _download

    dest = MODELS_DIR / f"{name}.onnx"
    if dest.exists():
        return dest
    archive = _download(MODELS_DIR / f"{name}.zip", url, sha256)
    with zipfile.ZipFile(archive) as z:
        member = next(m for m in z.namelist() if m.endswith("end2end.onnx"))
        partial = dest.with_suffix(".onnx.part")
        partial.write_bytes(z.read(member))
    partial.replace(dest)
    archive.unlink(missing_ok=True)
    return dest


class RTMPose:
    """One RTMPose-family ONNX model (SimCC output), run on a box."""

    def __init__(self, path: Path, size: tuple):
        import onnxruntime as ort

        self.session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
        self.input = self.session.get_inputs()[0].name
        self.size = size  # (width, height)

    def __call__(self, rgb: np.ndarray, box, padding: float = 1.25):
        """Points (K x 2, image px) and scores (K) for the box ``[x0, y0,
        x1, y1]``, widened by ``padding`` to the model's aspect ratio."""
        x0, y0, x1, y1 = box
        centre = np.array([(x0 + x1) / 2.0, (y0 + y1) / 2.0])
        scale = np.array([x1 - x0, y1 - y0], float) * padding
        w, h = self.size
        if scale[0] > scale[1] * w / h:
            scale = np.array([scale[0], scale[0] * h / w])
        else:
            scale = np.array([scale[1] * w / h, scale[1]])
        # Box centre to image centre, box width to the input's width.
        k = w / scale[0]
        m = np.array([[k, 0.0, w / 2.0 - k * centre[0]], [0.0, k, h / 2.0 - k * centre[1]]])
        img = cv2.warpAffine(rgb, m, (w, h), flags=cv2.INTER_LINEAR)
        # RGB with RGB means: MMPose trained on RGB (rtmlib feeds BGR here).
        x = ((img - MEAN) / STD).transpose(2, 0, 1)[None].astype(np.float32)
        simcc_x, simcc_y = self.session.run(None, {self.input: x})
        # SimCC: one score per half pixel of the input along each axis.
        locs = np.stack([simcc_x[0].argmax(1), simcc_y[0].argmax(1)], -1) / 2.0
        scores = 0.5 * (simcc_x[0].max(1) + simcc_y[0].max(1))
        xy = (locs - [w / 2.0, h / 2.0]) / k + centre
        return xy, scores


class RtmwHands:
    """RTMW on each person's box, then the hand model on each hand."""

    def __init__(self):
        # RTMW-l distilled from RTMW-x ("dw-x-l"), 384 x 288 input.
        body = _onnx_from_zip("rtmw-dw-x-l-384x288", RTMW_URL, RTMW_SHA256)
        self.body = RTMPose(body, (288, 384))
        self.hand = RTMPose(_onnx_from_zip("rtmpose-m-hand", HAND_URL, HAND_SHA256), (256, 256))

    def __call__(self, rgb: np.ndarray, people: list) -> list:
        hands = []
        for p in distinct_people(people):
            box = p["bbox_xyxy"]
            if box[2] - box[0] < 8 or box[3] - box[1] < 8:
                continue
            xy, sc = self.body(rgb, box)
            for side, first in RTMW_HANDS.items():
                hxy, hs = xy[first : first + 21], sc[first : first + 21]
                lo, hi = hxy.min(axis=0), hxy.max(axis=0)
                if hs.mean() < MIN_SCORE or (hi - lo).max() < 8:
                    continue
                rxy, rs = self.hand(rgb, [*lo, *hi], padding=1.6)
                # With the hands together the crop holds both, and the hand
                # model may pick the other: refined points are kept only on
                # the hand RTMW found.
                gap = np.linalg.norm(rxy.mean(axis=0) - hxy.mean(axis=0))
                if gap <= 0.3 * max(hand_size(hxy), 1.0):
                    hxy, hs = rxy, rs
                if hs.mean() < MIN_SCORE:
                    continue
                hands.append(
                    {
                        "subject_id": p.get("subject_id"),
                        "side": side,
                        "score": float(hs.mean()),
                        "keypoints": hxy.tolist(),
                        "scores": hs.tolist(),
                    }
                )
        return drop_repeats(hands)


class MediapipeHands:
    """MediaPipe's Hand Landmarker on a crop around each seen wrist."""

    CROP = 256  # the crop is resized to this for the landmarker

    def __init__(self):
        import mediapipe as mp
        from gaze.face_detect import _download

        self.mp = mp
        task = _download(MODELS_DIR / "hand_landmarker.task", MEDIAPIPE_URL, MEDIAPIPE_SHA256)
        opts = mp.tasks.vision.HandLandmarkerOptions(
            base_options=mp.tasks.BaseOptions(model_asset_path=str(task)),
            running_mode=mp.tasks.vision.RunningMode.IMAGE,
            # Two: with the hands together, both are in each wrist's crop.
            num_hands=2,
            min_hand_detection_confidence=0.4,
            min_hand_presence_confidence=0.4,
        )
        self.landmarker = mp.tasks.vision.HandLandmarker.create_from_options(opts)

    def __call__(self, rgb: np.ndarray, people: list) -> list:
        h, w = rgb.shape[:2]
        hands = []
        for centre, size in hand_seeds(people):
            half = int(1.1 * max(size, 100.0))
            x0, y0 = int(centre[0]) - half, int(centre[1]) - half
            sx0, sy0 = max(x0, 0), max(y0, 0)
            sx1, sy1 = min(x0 + 2 * half, w), min(y0 + 2 * half, h)
            if sx1 <= sx0 or sy1 <= sy0:
                continue
            crop = np.zeros((2 * half, 2 * half, 3), np.uint8)
            crop[sy0 - y0 : sy1 - y0, sx0 - x0 : sx1 - x0] = rgb[sy0:sy1, sx0:sx1]
            small = np.ascontiguousarray(cv2.resize(crop, (self.CROP, self.CROP)))
            image = self.mp.Image(image_format=self.mp.ImageFormat.SRGB, data=small)
            r = self.landmarker.detect(image)
            s = 2 * half
            for points, handedness in zip(r.hand_landmarks, r.handedness, strict=True):
                xy = [[p.x * s + x0, p.y * s + y0] for p in points]
                score = float(handedness[0].score)
                hands.append({"score": score, "keypoints": xy, "scores": [score] * len(xy)})
        return assign_by_wrist(drop_repeats(hands), people)


def make_estimator(name: str):
    """``"rtmw"`` or ``"mediapipe"``."""
    return {"rtmw": RtmwHands, "mediapipe": MediapipeHands}[name]()
