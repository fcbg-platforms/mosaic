"""
MediaPipe's metric canonical face, read straight from ``face_landmarker.task``.

The landmarker bundle (a zip) carries
``geometry_pipeline_metadata_landmarks.binarypb``: the 468-vertex canonical
face mesh in centimetres, plus the weighted "Procrustes basis", the
landmarks MediaPipe itself treats as rigid when it fits that mesh. This
module decodes the protobuf with a few lines of wire-format parsing, so it
needs neither the mediapipe face-geometry Python bindings (the wheel does not
ship them) nor a vendored copy of the model.

Vertex ``i`` of the mesh is landmark ``i`` (0..467). The ten iris landmarks
(468..477) are not part of the mesh.

The mesh is converted to the head frame used throughout
:mod:`gaze.ray_math`: millimetres, +x towards the subject's left, +y down,
+z into the head (MediaPipe's own frame is y up, z towards the viewer).
"""

from __future__ import annotations

import struct
import zipfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np

_METADATA_NAME = "geometry_pipeline_metadata_landmarks.binarypb"

# ── Landmark indices (MediaPipe 478-point face mesh) ─────────────────────────
# "Right"/"left" are the subject's own. The subject's right eye appears on
# the image left when they face the camera.
RIGHT_EYE_OUTER, RIGHT_EYE_INNER = 33, 133
RIGHT_EYE_TOP, RIGHT_EYE_BOTTOM = 159, 145
LEFT_EYE_INNER, LEFT_EYE_OUTER = 362, 263
LEFT_EYE_TOP, LEFT_EYE_BOTTOM = 386, 374
RIGHT_IRIS = (468, 469, 470, 471, 472)  # centre, then four rim points
LEFT_IRIS = (473, 474, 475, 476, 477)

# Points near the face's vertical midline: stable across views, so they are
# what cross-camera association compares.
MIDLINE_IDS = (168, 6, 197, 4, 1, 9, 151, 10)

# Rigid points used when MediaPipe's own Procrustes basis is unavailable:
# nose bridge and tip, forehead, eye corners, cheekbones and temples. Lids,
# brows, mouth and chin move with expression and are left out.
FALLBACK_RIGID_IDS = (
    168,
    6,
    197,
    195,
    5,
    4,
    1,
    10,
    151,
    9,
    8,
    109,
    338,
    67,
    297,
    33,
    133,
    362,
    263,
    116,
    345,
    123,
    352,
    50,
    280,
    127,
    356,
    234,
    454,
)


@dataclass(frozen=True)
class CanonicalFace:
    """The metric canonical face in the head frame.

    Attributes
    ----------
    vertices_mm : numpy.ndarray
        ``(468, 3)``, head frame, millimetres.
    rigid_ids : tuple of int
        Landmarks to fit head pose with.
    rigid_weights : numpy.ndarray
        One weight per ``rigid_ids`` entry (MediaPipe's own when available).
    """

    vertices_mm: np.ndarray
    rigid_ids: tuple
    rigid_weights: np.ndarray

    def points(self, ids) -> np.ndarray:
        """Model points for the given landmark ids, ``(len(ids), 3)``."""
        return self.vertices_mm[np.asarray(ids, dtype=int)]


def _varint(buf: bytes, i: int) -> tuple[int, int]:
    result = shift = 0
    while True:
        byte = buf[i]
        i += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, i


def _fields(buf: bytes) -> list[tuple[int, int, object]]:
    """Decode one protobuf message into ``(field, wire_type, value)``."""
    out = []
    i = 0
    while i < len(buf):
        key, i = _varint(buf, i)
        field, wire = key >> 3, key & 7
        if wire == 0:
            value, i = _varint(buf, i)
        elif wire == 1:
            value = struct.unpack_from("<d", buf, i)[0]
            i += 8
        elif wire == 2:
            length, i = _varint(buf, i)
            value = buf[i : i + length]
            i += length
        elif wire == 5:
            value = struct.unpack_from("<f", buf, i)[0]
            i += 4
        else:
            raise ValueError(f"unsupported protobuf wire type {wire}")
        out.append((field, wire, value))
    return out


def _packed_floats(value) -> list[float]:
    if isinstance(value, bytes):
        return list(struct.unpack(f"<{len(value) // 4}f", value))
    return [float(value)]


def parse_geometry_metadata(data: bytes) -> CanonicalFace:
    """Decode ``geometry_pipeline_metadata_landmarks.binarypb``.

    Parameters
    ----------
    data : bytes
        The file's contents.

    Returns
    -------
    CanonicalFace
        The mesh converted to the head frame, with MediaPipe's Procrustes
        basis as the rigid set (or :data:`FALLBACK_RIGID_IDS` if absent).

    Notes
    -----
    Layout (``GeometryPipelineMetadata``): field 1 is the canonical
    ``Mesh3d`` (its field 3 holds five floats per vertex: x, y, z, u, v, in
    centimetres), field 2 repeats ``WeightedLandmarkRef`` (field 1 landmark
    id, field 2 weight).
    """
    mesh_bytes = None
    basis: list[tuple[int, float]] = []
    for field, _wire, value in _fields(data):
        if field == 1 and isinstance(value, bytes):
            mesh_bytes = value
        elif field == 2 and isinstance(value, bytes):
            ref = dict((f, v) for f, _w, v in _fields(value))
            basis.append((int(ref.get(1, 0)), float(ref.get(2, 0.0))))
    if mesh_bytes is None:
        raise ValueError("no canonical mesh in the geometry metadata")

    floats: list[float] = []
    for field, _wire, value in _fields(mesh_bytes):
        if field == 3:
            floats.extend(_packed_floats(value))
    if len(floats) % 5 != 0 or len(floats) // 5 < 468:
        raise ValueError(f"unexpected canonical mesh size ({len(floats)} floats)")
    verts = np.asarray(floats, dtype=np.float64).reshape(-1, 5)[:468, :3]
    # MediaPipe: cm, y up, z towards the viewer. Head frame: mm, y down,
    # z into the head.
    verts_mm = verts * np.array([10.0, -10.0, -10.0])

    basis = [(i, w) for i, w in basis if 0 <= i < 468 and w > 0.0]
    if len(basis) >= 6:
        ids = tuple(i for i, _ in basis)
        weights = np.asarray([w for _, w in basis], dtype=np.float64)
    else:
        ids = FALLBACK_RIGID_IDS
        weights = np.ones(len(ids))
    return CanonicalFace(vertices_mm=verts_mm, rigid_ids=ids, rigid_weights=weights / weights.max())


def load_canonical_face(task_path: str | Path) -> CanonicalFace:
    """Read the canonical face out of a ``face_landmarker.task`` bundle."""
    with zipfile.ZipFile(task_path) as z:
        return parse_geometry_metadata(z.read(_METADATA_NAME))
