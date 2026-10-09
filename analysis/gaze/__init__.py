"""3D gaze for MOSAIC's Multi-Camera Gaze Fusion plugin (run_gaze_fusion.py).

Modules, from pixels to labels:

* :mod:`gaze.canonical`: MediaPipe's metric canonical face.
* :mod:`gaze.face_detect`: YuNet faces, then MediaPipe landmarks on crops.
* :mod:`gaze.head_pose`: metric head pose, per camera and joint across cameras.
* :mod:`gaze.eye_model`: eyeball model, from iris pixels to a visual axis.
* :mod:`gaze.fusion`: subjects across cameras and time, fused gaze per tick.
* :mod:`gaze.filters`: zero-lag smoothing.
* :mod:`gaze.targets`: what each gaze ray lands on.
* :mod:`gaze.timeline`: which frame of which video belongs to which tick.
* :mod:`gaze.render`: annotated videos, top-down room video, heat maps.
* :mod:`gaze.io_v2`: ``gaze_fusion.json``, CSV and summary.
* :mod:`gaze.ray_math`: the pure geometry everything above relies on.

Nothing is imported here, so ``import gaze.ray_math`` stays free of cv2 and
MediaPipe.
"""
