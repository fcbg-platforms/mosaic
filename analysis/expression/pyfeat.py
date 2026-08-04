"""
py-feat backend — Detectorv1 (FACS Action Units + 7-class emotion), the
most detailed but slowest of the 3 Facial Expression backends.

Deliberately Detectorv1, not the newer Detectorv2: Detectorv2's model
weights are HuggingFace-licensed "research-only, non-commercial use only";
Detectorv1's default au_model="xgb" + emotion_model="resmasknet" carry no
such restriction (only its optional identity_model="arcface" does, via
InsightFace's non-commercial terms — avoided below by passing
identity_model=None; this plugin never needs cross-frame identity/re-id).

Real-world caveats (verified against the installed py-feat==2.1.0 today,
not assumed from training-data memory — py-feat renamed Detector ->
Detectorv1 with no back-compat alias the same week this was researched):

  - `import feat` unconditionally imports torchcodec at module load time
    (feat/utils/io.py), which needs a torchcodec-compatible FFmpeg on
    PATH at *runtime* even though this module only ever passes single-
    frame in-memory tensors, never touches video I/O. This is unrelated
    to and separate from this project's own vcpkg-managed C++-side
    FFmpeg. If `import feat` fails, the error will be an FFmpeg-discovery
    RuntimeError, not a plain ImportError — check `where ffmpeg` first.
  - `feat/__init__.py` sets OMP_NUM_THREADS=1 process-wide as a side
    effect (a torch/xgboost OpenMP SIGSEGV workaround) unless already
    set. Harmless here since run_expression.py is its own subprocess,
    but documented in case it's ever seen in logs.
  - Model cache lives in site-packages/feat/resources (py-feat hardcodes
    cache_dir=get_resource_path() on every download, ignoring
    HF_HOME/HUGGINGFACE_HUB_CACHE) — NOT a project-local models/ dir
    like the other 2 expression backends. No .gitignore entry is needed
    or possible for this backend's cache.
  - CPU throughput is roughly 0.1-0.8s/frame (py-feat's own published
    benchmark dataset) — meaningfully slower than FER+'s near-instant
    ONNX inference. Surfaced via the UI combo's tooltip, not hidden.

The exact Detectorv1() constructor kwarg names below (identity_model,
device) and the "no face found" signal on the returned Fex frame
(fex.empty / the "FaceScore" column) MUST be re-verified against the
actually-installed package before relying on this in production:

    python -c "import inspect, feat; print(inspect.signature(feat.Detectorv1.__init__))"

py-feat is fast-moving and had a breaking rename (Detector -> Detectorv1)
the same day this module was written — do not trust these names as fact
without that check.
"""
from __future__ import annotations

import numpy as np

#: The 20 py-feat/Detectorv1 xgb-head Action Units (verified against
#: feat/pretrained.py's AU_LANDMARK_MAP["Feat"]). Values are a continuous
#: [0,1] calibrated probability, NOT the classic FACS 0-5 intensity scale.
AU_NAMES: list[str] = [
    "AU01", "AU02", "AU04", "AU05", "AU06", "AU07", "AU09", "AU10",
    "AU11", "AU12", "AU14", "AU15", "AU17", "AU20", "AU23", "AU24",
    "AU25", "AU26", "AU28", "AU43",
]

#: py-feat's own resmasknet emotion columns (lowercase in the Fex
#: DataFrame), mapped to a Title-Case display label — matches FER+'s own
#: "Happiness"/"Sadness" capitalization. Kept independent of classifier.py's
#: differently-named CATEGORIES ("Happy"/"Sad") on purpose: each backend
#: owns its own label vocabulary, same precedent FERPLUS_LABELS already set.
_EMOTION_COLUMN_TO_LABEL: dict[str, str] = {
    "anger": "Anger", "disgust": "Disgust", "fear": "Fear",
    "happiness": "Happiness", "sadness": "Sadness",
    "surprise": "Surprise", "neutral": "Neutral",
}


class PyFeatClassifier:
    """py-feat Detectorv1 backend — 20-AU + 7-class emotion.

    Loads/downloads Detectorv1's model weights once per process (py-feat's
    own cache, outside this project's control — see module docstring).
    """

    def __init__(self, device: str = "cpu") -> None:
        # Lazy import: keeps _fex_row_to_result() (and this whole module,
        # if nothing constructs a PyFeatClassifier) importable without
        # torch/torchcodec/feat installed — same reason ferplus.py defers
        # `import onnxruntime` into FerPlusClassifier.__init__ rather than
        # the module top.
        from feat import Detectorv1

        self._detector = Detectorv1(identity_model=None, device=device)

    def detect(self, face_crop_bgr: np.ndarray) -> tuple[str, float, dict[str, float]]:
        """Run AU + emotion detection on one already-cropped face image.

        Parameters
        ----------
        face_crop_bgr : numpy.ndarray
            A tight crop around one detected face (e.g. via
            :func:`~expression.detector.crop_bbox`) — the same
            already-cropped-image contract :meth:`FerPlusClassifier.classify`
            uses, so this backend can be wired into run_expression.py's
            per-face dispatch identically to the FER+ backend.

        Returns
        -------
        tuple of (str, float, dict[str, float])
            ``(dominant_emotion_label, dominant_score, au_values)`` —
            ``au_values`` keyed by :data:`AU_NAMES`, each in ``[0, 1]``.
            Returns ``("Neutral", 0.0, {})`` if py-feat's own internal
            face detector fails to find a face in the crop (a real, if
            rare, possibility on an already-tightly-cropped image — see
            the module docstring) rather than raising and aborting the
            whole analysis run, the same fail-soft precedent
            run_expression.py's own degenerate-bbox handling already uses.
        """
        tensor = _bgr_crop_to_tensor(face_crop_bgr)
        fex = self._detector.detect(tensor, data_type="tensor")

        if fex.empty or fex["FaceScore"].isna().all():
            return "Neutral", 0.0, {}

        au_row = fex.aus.iloc[0].to_dict()
        emotion_row = fex.emotions.iloc[0].to_dict()
        return _fex_row_to_result(au_row, emotion_row)


def _bgr_crop_to_tensor(face_crop_bgr: np.ndarray):
    """Convert one BGR numpy face crop into the (1,C,H,W) tensor
    ``Detectorv1.detect(..., data_type="tensor")`` expects.

    NOTE: the exact expected value range/dtype (uint8 0-255 vs. float
    0-1) for ``data_type="tensor"`` must be confirmed against
    Detectorv1's actual tensor-path preprocessing code at implementation
    time — not invented here.
    """
    import cv2
    import torch

    rgb = cv2.cvtColor(face_crop_bgr, cv2.COLOR_BGR2RGB)
    # transpose() only permutes strides, it never copies — wrapping that
    # directly in torch.from_numpy() hands torch a non-contiguous buffer,
    # which many torch/torchvision internals silently assume won't happen.
    # ascontiguousarray() forces a real copy into C-contiguous memory so
    # this is correct regardless of what Detectorv1's internals assume.
    chw = np.ascontiguousarray(rgb.transpose(2, 0, 1))
    return torch.from_numpy(chw).unsqueeze(0)


def _fex_row_to_result(au_values: dict, emotion_values: dict) -> tuple[str, float, dict[str, float]]:
    """Pure argmax-over-emotions + AU-passthrough — the one testable piece
    of this backend, isolated from the actual Detectorv1 call exactly like
    ferplus.py's ``_softmax_and_label()`` isolates softmax+argmax from the
    ONNX session call.

    Parameters
    ----------
    au_values : dict of str to float
        One row of py-feat's ``Fex.aus`` output, as a plain dict (e.g.
        ``fex.aus.iloc[0].to_dict()``).
    emotion_values : dict of str to float
        One row of py-feat's ``Fex.emotions`` output, as a plain dict.

    Returns
    -------
    tuple of (str, float, dict[str, float])
        ``(dominant_emotion_label, dominant_score, au_values)`` —
        ``dominant_emotion_label`` is one of :data:`_EMOTION_COLUMN_TO_LABEL`'s
        values (or the raw column name if unrecognized), ``au_values`` is
        keyed exactly as passed in, values rounded to 4 decimals.

    Notes
    -----
    NaN values (py-feat's own per-row failure marker when its internal
    models can't produce a score) are treated as ``0.0`` rather than
    propagating/crashing argmax — mirrors this codebase's established
    "default missing/bad data to 0.0, never crash" convention (e.g. the
    BLENDSHAPE_NAMES lookup-by-name-with-default on the detection side).
    """
    clean_emotions = {
        _EMOTION_COLUMN_TO_LABEL.get(name, name): (0.0 if _isnan(value) else float(value))
        for name, value in emotion_values.items()
    }
    best_label = max(clean_emotions, key=lambda k: clean_emotions[k]) if clean_emotions else "Neutral"
    best_score = clean_emotions.get(best_label, 0.0)

    au_dict = {
        name: (0.0 if _isnan(value) else round(float(value), 4))
        for name, value in au_values.items()
    }
    return best_label, best_score, au_dict


def _isnan(value) -> bool:
    return value != value  # NaN-safe without importing math for one check
