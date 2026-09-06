"""
Person-segmentation backend for MOSAIC's whole-body anonymization mode.

Kept separate from :mod:`facemask.detectors` rather than added as a fourth
entry there: the return type is categorically different (a frame-shaped
boolean raster, not a list of boxes), and ``run_face_mask.py`` maps
``expand_and_clip`` over every detector's output. Folding a raster-returning
backend into that Protocol would force a union return type every caller has
to re-narrow.
"""

from __future__ import annotations

from pathlib import Path
from typing import Protocol

import numpy as np

_MODELS_DIR = Path(__file__).parent / "models"

# Ultralytics' own recommended default for segmentation inference. Kept well
# below the face detector's 0.5: whole-body masking unions this with the face
# boxes, so the segmenter should be biased hard toward recall — a person it
# drops is a person whose body goes unmasked, and the face path is the only
# thing left to catch them.
DEFAULT_SEG_CONF = 0.25
DEFAULT_SEG_MODEL = "yolov8n-seg.pt"


class PersonSegmenter(Protocol):
    """Structural interface every person-segmentation backend satisfies.

    Duck-typed, exactly like :class:`facemask.detectors.FaceDetector`, so
    ``run_face_mask.py`` doesn't need to know which one is in use.
    """

    def segment(self, frame_bgr: np.ndarray) -> np.ndarray:
        """Segment every person in one BGR frame.

        Returns
        -------
        numpy.ndarray
            An ``(H, W)`` boolean array matching the frame's own height and
            width — the union of every detected person's silhouette. Already
            filtered by this backend's own confidence threshold.

            Pre-unioned rather than one mask per person: nothing downstream
            needs per-instance identity, and at 1080p with several people an
            ``(N, H, W)`` stack is several times the memory for no gain.
        """
        ...


def person_class_index(names: dict[int, str]) -> int:
    """Find the "person" class index in an ultralytics checkpoint's names map.

    Deliberately not hardcoded to 0. COCO happens to put person first, but a
    custom checkpoint passed via ``--seg-model`` need not, and masking the
    wrong class would fail silently and completely — every frame would be
    written out with the people untouched.

    Raises
    ------
    ValueError
        If the checkpoint has no "person" class at all, which means it is the
        wrong kind of model for this job.
    """
    for index, name in names.items():
        if str(name).strip().lower() == "person":
            return int(index)
    raise ValueError(
        "This checkpoint has no 'person' class, so it cannot be used for whole-body "
        f"anonymization. Classes: {sorted(str(n) for n in names.values())[:10]}"
    )


class YoloPersonSegmenter:
    """Whole-person instance segmentation via an ultralytics YOLOv8-seg model.

    Parameters
    ----------
    model : str or None, default None
        A local checkpoint path or an official ultralytics segmentation model
        name; ``None`` means :data:`DEFAULT_SEG_MODEL`.
    conf_threshold : float, default :data:`DEFAULT_SEG_CONF`
        Minimum confidence to keep an instance.
    device : str or None, default None
        Inference device (``"cpu"``, ``"cuda:0"``); ``None`` lets ultralytics
        choose.
    imgsz : int, default 640
        Inference resolution. Larger is slower but resolves small/distant
        people better.
    """

    def __init__(
        self,
        model: str | None = None,
        conf_threshold: float = DEFAULT_SEG_CONF,
        device: str | None = None,
        imgsz: int = 640,
    ) -> None:
        from ultralytics import YOLO

        model_name = model or DEFAULT_SEG_MODEL
        # An absolute path under models/ rather than a bare name: ultralytics'
        # attempt_download_asset() recognises its own official asset names and
        # downloads to exactly the path given, via a .part file published only
        # after the size check. That keeps the weights beside the face models
        # this package already caches, instead of wherever the process happens
        # to be running.
        model_path = model_name if Path(model_name).is_file() else str(_MODELS_DIR / model_name)

        self._conf = conf_threshold
        self._device = device
        self._imgsz = imgsz
        self._model = YOLO(model_path)
        self._person_class = person_class_index(self._model.names)

    def segment(self, frame_bgr: np.ndarray) -> np.ndarray:
        """See :meth:`PersonSegmenter.segment`."""
        frame_h, frame_w = frame_bgr.shape[:2]
        union = np.zeros((frame_h, frame_w), dtype=bool)

        results = self._model(
            frame_bgr,
            conf=self._conf,
            device=self._device,
            imgsz=self._imgsz,
            classes=[self._person_class],
            # Load-bearing. Without it ultralytics returns masks at the
            # LETTERBOXED MODEL INPUT size (see SegmentationPredictor.
            # construct_result) while scaling the boxes back to the original
            # frame — two different coordinate spaces. Resizing such a mask to
            # the frame stretches the padding bands across it and shifts every
            # silhouette sideways, which renders as a perfectly plausible
            # person-shaped blur beside a partly-exposed person.
            retina_masks=True,
            verbose=False,
        )

        for res in results:
            if res.masks is None:
                continue
            data = res.masks.data
            if data.shape[0] == 0:
                continue
            # Belt and braces for the above: if a future ultralytics changes
            # what retina_masks returns, fail loudly here rather than silently
            # mask the wrong pixels. run_face_mask.py turns any exception into
            # a blacked-out frame, so the failure mode is over-masking.
            if tuple(data.shape[1:]) != (frame_h, frame_w):
                raise RuntimeError(
                    f"segmentation mask shape {tuple(data.shape[1:])} does not match frame "
                    f"{(frame_h, frame_w)} — refusing to guess how to align it"
                )
            union |= data.any(0).cpu().numpy().astype(bool)

        return union


def make_segmenter(
    model: str | None = None,
    conf_threshold: float = DEFAULT_SEG_CONF,
    device: str | None = None,
    imgsz: int = 640,
) -> PersonSegmenter:
    """Build a person-segmentation backend.

    Only one backend exists today; the factory mirrors
    :func:`facemask.detectors.make_detector` so adding a second (e.g. SAM 2 for
    cleaner mask edges) doesn't change any call site.
    """
    return YoloPersonSegmenter(
        model=model, conf_threshold=conf_threshold, device=device, imgsz=imgsz
    )
