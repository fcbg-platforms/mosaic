"""Face-ROI extraction, classical signal-combination algorithms (Green/
CHROM/POS), and Welch-periodogram heart-rate estimation for MOSAIC's
Remote Heart Rate (rPPG) analysis plugin — EXPERIMENTAL, research-grade
only, not a medical device."""
from .algorithms import BACKENDS, chrom_signal, green_signal, normalize_channels, pos_signal
from .hr_estimation import bandpass_filter, estimate_hr_welch, median_smooth
from .roi import FaceRoiSample, MediaPipeFaceRoiExtractor

__all__ = [
    "normalize_channels",
    "green_signal",
    "chrom_signal",
    "pos_signal",
    "BACKENDS",
    "bandpass_filter",
    "estimate_hr_welch",
    "median_smooth",
    "FaceRoiSample",
    "MediaPipeFaceRoiExtractor",
]
