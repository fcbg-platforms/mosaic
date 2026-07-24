"""
FER+ backend — a dedicated pretrained facial-expression-recognition model,
offered as a more validated alternative to classifier.py's rule-based
heuristic (mirrors facemask's multi-backend pattern).

Model: Microsoft's "emotion-ferplus" (opset 8), from the ONNX Model Zoo
(onnx/models), MIT-licensed, trained on the FER+ dataset (a relabeled/
cleaned version of the FER2013 Kaggle challenge). Verified directly against
the model card and the upstream training repo (github.com/ebarsoum/FERPlus)
rather than assumed from memory, since a wrong label-index mapping would
silently mislabel every prediction:

  - Input:  (1, 1, 64, 64) grayscale, RAW 0-255 float32 pixel values — no
            normalization (no /255, no mean subtraction). Confirmed from
            the model card's own reference preprocessing code.
  - Output: (1, 8) raw logits — the ONNX graph does NOT include a softmax;
            the caller must apply one.
  - Labels (index 0-7): neutral, happiness, surprise, sadness, anger,
            disgust, fear, contempt. Cross-verified against both the
            onnx/models README and the original FERPlus training repo's
            CSV column order (two independent primary sources agree). Do
            not confuse this with the base FER2013 dataset's own label
            order (angry/disgust/fear/happy/sad/surprise/neutral) — a
            historical GitHub issue conflating the two was investigated
            and appears to be a red herring, not evidence this model's
            documented order is wrong.

The model file is Git-LFS-tracked in its GitHub repo — the naive
raw.githubusercontent.com URL silently serves a 133-byte LFS pointer stub
instead of the actual binary. This module downloads from
media.githubusercontent.com (GitHub's LFS media-content endpoint) instead,
and verifies the download's sha256 against the known-good hash so a
corrupted/truncated/wrong download fails loudly rather than silently
producing garbage predictions.
"""
from __future__ import annotations

import hashlib
import math
import urllib.request
from pathlib import Path

import cv2
import numpy as np

_MODELS_DIR = Path(__file__).parent / "models"

_FERPLUS_MODEL_URL = (
    "https://media.githubusercontent.com/media/onnx/models/main/"
    "validated/vision/body_analysis/emotion_ferplus/model/emotion-ferplus-8.onnx"
)
_FERPLUS_MODEL_SHA256 = (
    "a2a2ba6a335a3b29c21acb6272f962bd3d47f84952aaffa03b60986e04efa61c"
)

#: Official FER+ label order (see module docstring) — index *i* is the
#: emotion for the ONNX model's *i*-th output logit.
FERPLUS_LABELS: list[str] = [
    "Neutral", "Happiness", "Surprise", "Sadness",
    "Anger", "Disgust", "Fear", "Contempt",
]


class FerPlusClassifier:
    """Microsoft FER+ ONNX model backend — 8-category emotion classification.

    Downloads and sha256-verifies ``emotion-ferplus-8.onnx`` to ``models/``
    on first use (see :func:`_ensure_download_verified`).
    """

    def __init__(self) -> None:
        import onnxruntime as ort

        model_path = _ensure_download_verified(
            _MODELS_DIR / "emotion-ferplus-8.onnx",
            _FERPLUS_MODEL_URL, _FERPLUS_MODEL_SHA256)

        self._session = ort.InferenceSession(str(model_path))
        self._input_name = self._session.get_inputs()[0].name

    def classify(self, face_crop_bgr: np.ndarray) -> tuple[str, float]:
        """Classify one face crop's dominant emotion.

        Parameters
        ----------
        face_crop_bgr : numpy.ndarray
            A tight crop around one detected face (e.g. via
            :func:`~expression.detector.crop_bbox`). The model card gives
            no explicit crop/alignment guidance, but FER2013/FER+'s
            source data is already tightly-cropped near-square face
            images, so a loose/full-scene frame would be a real
            preprocessing mismatch against training.

        Returns
        -------
        tuple of (str, float)
            ``(label, score)`` — ``label`` is one of :data:`FERPLUS_LABELS`,
            ``score`` the softmax probability of that label.
        """
        gray = cv2.cvtColor(face_crop_bgr, cv2.COLOR_BGR2GRAY)
        resized = cv2.resize(gray, (64, 64))
        # No normalization — the model expects raw 0-255 pixel intensities.
        input_tensor = resized.astype(np.float32).reshape(1, 1, 64, 64)

        logits = self._session.run(None, {self._input_name: input_tensor})[0][0]
        return _softmax_and_label(logits.tolist(), FERPLUS_LABELS)


def _softmax_and_label(logits: list[float], labels: list[str]) -> tuple[str, float]:
    """Apply a numerically-stable softmax and pick the argmax label.

    Parameters
    ----------
    logits : list of float
        Raw model output logits, parallel to ``labels``.
    labels : list of str
        Class labels, parallel to ``logits`` (e.g. :data:`FERPLUS_LABELS`).

    Returns
    -------
    tuple of (str, float)
        ``(label, score)`` for the highest-probability class.

    Notes
    -----
    Pulled out as a pure function specifically so it's unit-testable
    without onnxruntime/the model file present (same "extract the one
    pure/testable piece" pattern :func:`~diarize.pipeline.resolve_device`
    established). Subtracts ``max(logits)`` before exponentiating for
    numerical stability. See :doc:`/math/facial_expression` for the
    formula.
    """
    max_logit = max(logits)
    exps = [math.exp(v - max_logit) for v in logits]
    total = sum(exps)
    probs = [e / total for e in exps]

    best_index = max(range(len(probs)), key=lambda i: probs[i])
    return labels[best_index], probs[best_index]


def _ensure_download_verified(dest: Path, url: str, expected_sha256: str) -> Path:
    """Download ``url`` to ``dest`` if not already cached, verifying its sha256.

    Parameters
    ----------
    dest : pathlib.Path
        Destination file path; parent directories are created as needed.
    url : str
        Source URL.
    expected_sha256 : str
        Expected hex-digest sha256 of the downloaded file.

    Returns
    -------
    pathlib.Path
        ``dest``, unchanged.

    Raises
    ------
    RuntimeError
        If the downloaded file's sha256 doesn't match
        ``expected_sha256`` — the bad download is deleted rather than
        cached, so a truncated/corrupted file can't silently produce
        garbage predictions on a later run.
    """
    if dest.exists() and _sha256_of(dest) == expected_sha256:
        return dest

    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"[expression] Downloading {dest.name} …", flush=True)
    urllib.request.urlretrieve(url, dest)

    actual = _sha256_of(dest)
    if actual != expected_sha256:
        dest.unlink(missing_ok=True)
        raise RuntimeError(
            f"Downloaded {dest.name} but its sha256 ({actual}) doesn't match the "
            f"expected {expected_sha256} — the download may have been truncated, "
            f"corrupted, or (if the model is ever re-exported upstream) the pinned "
            f"hash in ferplus.py needs updating. Deleted the bad download; not "
            f"caching a file that might produce garbage predictions."
        )
    print(f"[expression] Downloaded to {dest}", flush=True)
    return dest


def _sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()
