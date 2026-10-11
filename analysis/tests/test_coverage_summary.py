"""scripts/coverage_summary.py: the CI coverage table, from both report kinds."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import coverage_summary as cs


def write(tmp_path, body):
    p = tmp_path / "report.xml"
    p.write_text(f'<?xml version="1.0" ?><coverage><packages>{body}</packages></coverage>')
    return str(p)


def cls(filename, hits):
    lines = "".join(f'<line number="{i + 1}" hits="{h}"/>' for i, h in enumerate(hits))
    return f'<class filename="{filename}"><lines>{lines}</lines></class>'


def test_python_paths_group_by_plugin_folder(tmp_path):
    report = write(
        tmp_path,
        "<package><classes>"
        + cls("gaze2d/iris_offset.py", [1, 1, 0, 0])
        + cls("gaze2d/estimator.py", [0, 0])
        + cls("run_gaze2d.py", [1, 0])
        + "</classes></package>",
    )
    text = cs.summarise(cs.read_lines(report, None), "Python")
    assert "**37.5%** of 8 lines" in text
    assert "| `gaze2d/` | 6 | 2 | 33.3% |" in text
    assert "| `(top level)` | 2 | 1 | 50.0% |" in text


def test_cpp_paths_are_made_relative_and_merged_across_modules(tmp_path):
    # OpenCppCoverage: absolute Windows paths, the same file once per module.
    a = cls(r"D:\a\mosaic\mosaic\src\core\settings.cpp", [1, 0, 0])
    b = cls(r"D:\a\mosaic\mosaic\src\core\settings.cpp", [0, 1, 0])
    c = cls(r"D:\a\mosaic\mosaic\src\session\session_archive.cpp", [1, 1])
    report = write(
        tmp_path,
        f"<package><classes>{a}</classes></package>"
        f"<package><classes>{b}{c}</classes></package>",
    )
    files = cs.read_lines(report, "src")
    assert set(files) == {"core/settings.cpp", "session/session_archive.cpp"}
    assert files["core/settings.cpp"] == {1: True, 2: True, 3: False}
    text = cs.summarise(files, "C++")
    assert "| `core/` | 3 | 2 | 66.7% |" in text
    assert "| `session/` | 2 | 2 | 100.0% |" in text


def test_least_covered_lists_only_big_files(tmp_path):
    big = cls("report/render.py", [0] * 30 + [1] * 10)
    small = cls("report/tiny.py", [0, 0])
    text = cs.summarise(
        cs.read_lines(write(tmp_path, f"<package><classes>{big}{small}</classes></package>"), None),
        "x",
    )
    assert "`report/render.py` | 40 | 25.0%" in text
    assert "tiny.py" not in text.split("Least covered")[1]


def test_an_empty_report_says_so(tmp_path, capsys):
    report = write(tmp_path, "")
    assert cs.main([report, "--title", "C++"]) == 0
    assert "lists no files" in capsys.readouterr().out
