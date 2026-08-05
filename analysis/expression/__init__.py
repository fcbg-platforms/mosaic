"""Facial-expression detection and classification backends (rule-based
heuristic + FER+ ONNX + py-feat) for MOSAIC's Facial Expression analysis
plugin."""
from .classifier import CATEGORIES, CATEGORY_WEIGHTS, classify_expression
from .detector import BLENDSHAPE_NAMES, FaceExpression, MediaPipeExpressionDetector, crop_bbox
from .ferplus import FERPLUS_LABELS, FerPlusClassifier
from .pyfeat import AU_NAMES, PyFeatClassifier

__all__ = [
    "classify_expression",
    "CATEGORIES",
    "CATEGORY_WEIGHTS",
    "FaceExpression",
    "MediaPipeExpressionDetector",
    "BLENDSHAPE_NAMES",
    "crop_bbox",
    "FerPlusClassifier",
    "FERPLUS_LABELS",
    "PyFeatClassifier",
    "AU_NAMES",
]
