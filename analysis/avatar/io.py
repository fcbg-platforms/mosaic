"""The avatar files the app reads: per person one binary, laid out as
``avatar.json`` says, so that MOSAIC draws the body without the body model.

Each person's ``avatar/person_N.bin`` (little-endian), in this order:

1. ``rest_vertices``: float32, V x 3, the body at rest in the fit's frame
   (metres, z up);
2. ``faces``: uint32, F x 3;
3. ``skin_bones``: uint16, V x K, and ``skin_weights``: float32, V x K: the
   bones moving each vertex and their weights (linear blend skinning);
4. ``time_ms``: float64, N, each tick's session time;
5. ``valid``: uint32, N, a bit per 3D Pose keypoint that was measured;
6. ``bones``: float32, N x J x 7, each bone's skinning transform per tick as
   a unit quaternion (x, y, z, w) and a translation (metres): a vertex is
   ``sum_k w_k (R_k v + t_k)``.

The fit's frame goes to the room's (millimetres) by ``to_room``: a 3 x 3
matrix (rotation times 1000), the same for every person.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np

SCHEMA = "mosaic-avatar-v1"
LAYOUT = (
    ("rest_vertices", np.float32, 3),
    ("faces", np.uint32, 3),
    ("skin_bones", np.uint16, None),
    ("skin_weights", np.float32, None),
    ("time_ms", np.float64, 0),
    ("valid", np.uint32, 0),
    ("bones", np.float32, 7),
)


def write_person(path: Path, arrays: dict) -> dict:
    """Writes one person's arrays (see the module docstring) in order;
    returns the counts ``avatar.json`` records for them."""
    counts = {
        "vertices": int(arrays["rest_vertices"].shape[0]),
        "faces": int(arrays["faces"].shape[0]),
        "weights_per_vertex": int(arrays["skin_bones"].shape[1]),
        "ticks": int(arrays["time_ms"].shape[0]),
        "bone_count": int(arrays["bones"].shape[1]),
    }
    partial = path.with_name(path.name + ".partial")
    with partial.open("wb") as f:
        for name, dtype, _cols in LAYOUT:
            f.write(np.ascontiguousarray(arrays[name], dtype=dtype).tobytes())
    partial.replace(path)
    counts["bytes"] = path.stat().st_size
    return counts


def read_person(path: Path, counts: dict) -> dict:
    """The arrays of one person's file (for tests and checks)."""
    v, f, k = counts["vertices"], counts["faces"], counts["weights_per_vertex"]
    n, j = counts["ticks"], counts["bone_count"]
    shapes = {
        "rest_vertices": (v, 3),
        "faces": (f, 3),
        "skin_bones": (v, k),
        "skin_weights": (v, k),
        "time_ms": (n,),
        "valid": (n,),
        "bones": (n, j, 7),
    }
    data = path.read_bytes()
    out, pos = {}, 0
    for name, dtype, _cols in LAYOUT:
        count = int(np.prod(shapes[name]))
        size = count * np.dtype(dtype).itemsize
        out[name] = np.frombuffer(data, dtype=dtype, count=count, offset=pos).reshape(shapes[name])
        pos += size
    if pos != len(data):
        raise ValueError(f"{path.name}: {len(data)} bytes, the layout says {pos}")
    return out
