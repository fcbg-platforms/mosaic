"""Per-camera 3D gaze-ray estimation and multi-camera ray-fusion math for
MOSAIC's Multi-Camera Gaze Fusion analysis plugin."""

from .estimator import FaceGazeSample, MediaPipeGazeEstimator3D
from .ray_math import (
    camera_ray_from_pose,
    closest_point_of_rays,
    ray_plane_intersection,
    transform_ray_to_room,
)

__all__ = [
    "camera_ray_from_pose",
    "transform_ray_to_room",
    "closest_point_of_rays",
    "ray_plane_intersection",
    "FaceGazeSample",
    "MediaPipeGazeEstimator3D",
]
