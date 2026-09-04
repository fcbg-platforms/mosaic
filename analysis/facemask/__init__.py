"""Face-detection backends and blur/box masking helpers for MOSAIC's
Face Masking analysis plugin."""

from .detectors import (
    Box,
    FaceDetector,
    MediaPipeFaceDetector,
    OpenCVDnnFaceDetector,
    YoloFaceDetector,
    make_detector,
)
from .masking import (
    apply_mask,
    apply_mask_region,
    dilate_mask,
    expand_and_clip,
    rasterize_boxes,
    region_blur_kernel,
)
from .segmenters import (
    DEFAULT_SEG_CONF,
    DEFAULT_SEG_MODEL,
    PersonSegmenter,
    YoloPersonSegmenter,
    make_segmenter,
    person_class_index,
)

__all__ = [
    "Box",
    "FaceDetector",
    "MediaPipeFaceDetector",
    "YoloFaceDetector",
    "OpenCVDnnFaceDetector",
    "make_detector",
    "expand_and_clip",
    "apply_mask",
    "apply_mask_region",
    "dilate_mask",
    "rasterize_boxes",
    "region_blur_kernel",
    "PersonSegmenter",
    "YoloPersonSegmenter",
    "make_segmenter",
    "person_class_index",
    "DEFAULT_SEG_CONF",
    "DEFAULT_SEG_MODEL",
]
