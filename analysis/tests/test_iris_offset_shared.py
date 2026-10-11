"""
The 2D gaze heuristic lives in iris_offset.py, kept byte for byte identical in
python/pose/ (the live gaze arrow and on-target figure) and analysis/gaze2d/
(the Gaze 2D plugin), because the two are separate uv projects. These tests
fail if the copies drift apart, or if the on-target rule stops matching the
C++ one the Real-time tab uses (src/analysis/realtime_metrics.*).
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from gaze2d.iris_offset import GAZE_ON_TARGET_THRESHOLD, gaze_on_target

REPO = Path(__file__).resolve().parents[2]
ANALYSIS_COPY = REPO / "analysis" / "gaze2d" / "iris_offset.py"
LIVE_COPY = REPO / "python" / "pose" / "iris_offset.py"


def test_the_live_and_analysis_copies_are_identical():
    # Compared as text with line endings normalised (git may check either
    # copy out with CRLF on Windows).
    a = ANALYSIS_COPY.read_text(encoding="utf-8").replace("\r\n", "\n")
    b = LIVE_COPY.read_text(encoding="utf-8").replace("\r\n", "\n")
    assert a == b, "python/pose/iris_offset.py and analysis/gaze2d/iris_offset.py differ"


def test_the_threshold_matches_the_cpp_constant():
    header = (REPO / "src" / "analysis" / "realtime_metrics.hpp").read_text(encoding="utf-8")
    m = re.search(r"kGazeOnTargetThreshold\s*=\s*([0-9.]+)\s*;", header)
    assert m, "kGazeOnTargetThreshold not found in realtime_metrics.hpp"
    assert float(m.group(1)) == GAZE_ON_TARGET_THRESHOLD


def test_the_cpp_rule_is_also_a_radius():
    source = (REPO / "src" / "analysis" / "realtime_metrics.cpp").read_text(encoding="utf-8")
    body = source[source.index("bool gaze_on_target_for(") :]
    body = body[: body.index("}")]
    assert "std::hypot(gazeDx, gazeDy) <= threshold" in body
    # ...which the Python rule mirrors.
    assert gaze_on_target(0.2, -0.28)
    assert not gaze_on_target(0.3, 0.25)
