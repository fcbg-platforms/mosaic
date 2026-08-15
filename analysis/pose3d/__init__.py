"""Multi-view DLT triangulation, cross-camera person association, and 3D
track identity for MOSAIC's 3D Pose Reconstruction analysis plugin."""
from .association import (
    PersonObservation,
    cluster_people,
    match_camera_pair,
    pairwise_cost,
)
from .tracker import PersonTracker3D, TrackedPerson3D
from .triangulation import (
    CameraGeom,
    TriangulationResult,
    invert_rt,
    normalize_point,
    project_point_px,
    projection_matrix,
    reproject_error_px,
    triangulate_point_dlt,
    triangulate_with_rejection,
)

__all__ = [
    "CameraGeom",
    "TriangulationResult",
    "invert_rt",
    "normalize_point",
    "projection_matrix",
    "project_point_px",
    "triangulate_point_dlt",
    "reproject_error_px",
    "triangulate_with_rejection",
    "PersonObservation",
    "pairwise_cost",
    "match_camera_pair",
    "cluster_people",
    "TrackedPerson3D",
    "PersonTracker3D",
]
