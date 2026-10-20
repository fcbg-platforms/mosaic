"""Finding and following objects named in words, with SAM 3.

SAM 3 (Meta, 2025; run through ultralytics) takes short noun phrases such as
"toy" or "cup", finds every instance in the first frames, and follows each
through the video with its own id, adding instances that appear later. Its
weights (``sam3.pt``) are gated: download them from Hugging Face after accepting
Meta's SAM licence, and put them in the MOSAIC folder.

This module is the only place that touches the model, so the rest of Object
Tracking can be tested without it.
"""

from __future__ import annotations

from pathlib import Path

from .contact import outline

#: The score a detection needs to start a new object. SAM 3's own 0.5 let
#: brief false objects through on a first room-11 recording (a window behind
#: the person as a "tablet", at scores of 0.5 to 0.6); objects already
#: followed are kept down to ``conf``.
NEW_OBJECT_SCORE = 0.6


def track_video(video: Path, labels: list, weights: Path, stride: int = 2, conf: float = 0.4):
    """Every ``stride``-th frame of ``video``: the objects found.

    Yields ``(frame_index, objects)`` with ``objects`` a list of ``{"id",
    "label", "score", "box", "polygon"}`` (box ``[x1, y1, x2, y2]`` and polygon
    in pixels). ``frame_index`` counts decoded frames from 0: frames
    ``stride - 1``, ``2 * stride - 1``, and so on.
    """
    import torch
    from ultralytics.models.sam import SAM3VideoSemanticPredictor

    predictor = SAM3VideoSemanticPredictor(
        overrides={
            "conf": conf,
            "task": "segment",
            "mode": "predict",
            "model": str(weights),
            "vid_stride": stride,
            "quantize": 16 if torch.cuda.is_available() else None,  # fp16 on a GPU only
            "verbose": False,
            "save": False,
        },
        new_det_thresh=NEW_OBJECT_SCORE,
    )
    results = predictor(source=str(video), text=list(labels), stream=True)
    for k, r in enumerate(results):
        objects = []
        boxes = r.boxes
        masks = r.masks.data.cpu().numpy() if r.masks is not None else []
        if boxes is not None and boxes.id is not None:
            ids = boxes.id.int().tolist()
            cls = boxes.cls.int().tolist()
            score = boxes.conf.tolist()
            xyxy = boxes.xyxy.tolist()
            for i, oid in enumerate(ids):
                objects.append(
                    {
                        "id": int(oid),
                        "label": labels[cls[i]] if 0 <= cls[i] < len(labels) else str(cls[i]),
                        "score": round(float(score[i]), 3),
                        "box": [round(float(v), 1) for v in xyxy[i]],
                        "polygon": outline(masks[i] > 0.5) if i < len(masks) else [],
                    }
                )
        # With a stride the loader reads `stride` frames and keeps the last.
        yield k * stride + stride - 1, objects
