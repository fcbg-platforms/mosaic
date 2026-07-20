"""
Sphinx configuration for MOSAIC documentation.

Build pipeline:
    1. CMake (or ``doxygen docs/Doxyfile.in``) produces Doxygen XML.
    2. Breathe reads the XML and exposes C++ entities to Sphinx.
    3. Exhale walks the Breathe tree and auto-generates RST API pages.
    4. Sphinx renders everything to HTML (or PDF via LaTeX).

Quickstart::

    cd docs && uv sync && cd ..
    cmake -S . -B build/Debug -DMOSAIC_BUILD_DOCS=ON
    cmake --build build/Debug --target docs
    # then open build/Debug/docs/sphinx/html/index.html

Or without CMake::

    doxygen docs/Doxyfile.in   # produces build/Debug/doxygen/xml (or ./doxygen/xml)
    cd docs
    DOXYGEN_XML=../build/Debug/doxygen/xml sphinx-build -b html . _build/html
"""

import os
import sys
import textwrap
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
docs_dir = Path(__file__).parent
project_root = docs_dir.parent
sys.path.insert(0, str(project_root))
# analysis/ subpackages import as top-level `pose`, `facemask`, `diarize`,
# `expression`, `gaze`, `motion` — same convention analysis/tests/*.py uses
# (sys.path.insert(0, str(Path(__file__).parent.parent)) from a file inside
# analysis/tests/), so autodoc needs analysis/ itself on sys.path, not
# project_root/analysis as a package.
sys.path.insert(0, str(project_root / "analysis"))

# Doxygen XML location: prefer the CMake-built path, fall back to a local one.
_doxygen_xml = os.environ.get(
    "DOXYGEN_XML",
    str(project_root / "build" / "Debug" / "doxygen" / "xml"),
)

# ---------------------------------------------------------------------------
# Project metadata
# ---------------------------------------------------------------------------
project   = "MOSAIC"
author    = "CSRU Lab"
copyright = "2024–2026, CSRU Lab"
version   = "0.1"
release   = "0.1.0"

# ---------------------------------------------------------------------------
# Extensions
# ---------------------------------------------------------------------------
extensions = [
    "breathe",
    "exhale",
    "sphinx.ext.autodoc",       # Python API reference (analysis/ package)
    "sphinx.ext.autosummary",
    "sphinx.ext.napoleon",      # numpydoc-style docstring rendering
    "sphinx.ext.mathjax",       # .. math:: rendering for docs/math/*
    "myst_parser",              # Markdown support
    "sphinx.ext.intersphinx",
    "sphinx.ext.viewcode",
    "sphinx.ext.autosectionlabel",
    "sphinx_copybutton",
    "sphinx_design",
]

# ---------------------------------------------------------------------------
# Breathe — C++ API bridge
# ---------------------------------------------------------------------------
breathe_projects          = {"MOSAIC": _doxygen_xml}
breathe_default_project   = "MOSAIC"
breathe_default_members   = ("members", "undoc-members")
breathe_show_include      = True
breathe_order_parameters_first = True

# ---------------------------------------------------------------------------
# Exhale — auto-generate RST API tree
# ---------------------------------------------------------------------------
exhale_args = {
    # where Exhale writes its generated RST files
    "containmentFolder":     "./api",
    "rootFileName":          "library_root.rst",
    "rootFileTitle":         "API Reference",
    "doxygenStripFromPath":  str(project_root / "src"),

    # page titles
    "createTreeView":        True,
    "treeViewIsBootstrap":   False,

    # Show the full signature on every function page
    "exhaleExecutesDoxygen": False,   # we run Doxygen via CMake

    # Link to source in a "View source" button
    "contentsDirectives": True,
}

# ---------------------------------------------------------------------------
# Napoleon — render numpydoc-style docstrings (analysis/ package)
# ---------------------------------------------------------------------------
napoleon_google_docstring         = False
napoleon_numpy_docstring          = True
napoleon_include_init_with_doc    = True
napoleon_include_private_with_doc = False
napoleon_include_special_with_doc = False
napoleon_use_param                = True
napoleon_use_rtype                = True
napoleon_use_ivar                 = False
napoleon_attr_annotations         = True

# ---------------------------------------------------------------------------
# Autodoc / Autosummary — Python analysis/ API reference (docs/analysis_api.rst)
# ---------------------------------------------------------------------------
autodoc_typehints    = "description"   # keep signatures short, types in the body
autodoc_member_order = "bysource"
autosummary_generate = True
autosummary_generate_overwrite = True

# analysis/ imports heavy/optional ML deps at module level (cv2, mediapipe,
# torch, ...) — mocked so autodoc can import every module for docstring
# introspection WITHOUT installing the full ML stack in the docs venv.
# numpy is deliberately NOT mocked: it's small/CPU-only/fast to install, and
# nearly every analysis/ signature involves np.ndarray — mocking it would
# render every such type as a fake MagicMock instead of a real,
# intersphinx-linked numpy.ndarray.
autodoc_mock_imports = [
    "cv2",
    "mediapipe",
    "ultralytics",
    "torch",
    "torchvision",
    "pyannote",
    "pyannote.audio",
    "onnxruntime",
    "faster_whisper",
    "scipy",
    "h5py",
    "pandas",
    "matplotlib",
    "matplotlib.pyplot",
]

# ---------------------------------------------------------------------------
# MathJax — .. math:: rendering for docs/math/*
# ---------------------------------------------------------------------------
mathjax3_config = {
    "tex": {
        "tags": "ams",   # numbered \tag{}/equation environments, AMS-style
    },
}

# ---------------------------------------------------------------------------
# MyST — Markdown options
# ---------------------------------------------------------------------------
myst_enable_extensions = [
    "colon_fence",      # ::: fences like RST directives
    "deflist",          # definition lists
    "fieldlist",
    "substitution",
]
myst_heading_anchors = 3

# ---------------------------------------------------------------------------
# Theme — pydata-sphinx-theme (NumPy / SciPy / pandas / matplotlib standard)
# ---------------------------------------------------------------------------
html_theme = "pydata_sphinx_theme"

html_title       = "MOSAIC"
html_short_title = "MOSAIC"

html_theme_options = {
    "logo": {
        # Placeholder mark — see docs/_static/logo-{light,dark}.svg's own
        # comment. Swap these two files for finalized brand assets whenever
        # they exist; the theme wiring itself won't need to change.
        "image_light": "_static/logo-light.svg",
        "image_dark":  "_static/logo-dark.svg",
        "text": "MOSAIC",
        "alt_text": "MOSAIC — Multi-camera Observatory for Social & Activity Interaction Capture",
    },
    "navbar_align":  "left",
    "show_nav_level": 2,
    "navigation_depth": 3,
    "show_toc_level": 2,
    "header_links_before_dropdown": 6,

    # Footer
    "footer_start": ["copyright"],
    "footer_center": [],
    "footer_end":   ["sphinx-version"],

    # Syntax highlighting
    "pygments_light_style": "friendly",
    "pygments_dark_style":  "monokai",

    # Article-level sidebar
    "primary_sidebar_end": ["indices.html"],
    "secondary_sidebar_items": ["page-toc", "edit-this-page"],

    # Announcement banner (optional — remove if not needed)
    # "announcement": "MOSAIC is under active development.",
}

html_static_path   = ["_static"]
html_css_files     = ["custom.css"]
html_favicon       = "_static/logo-light.svg"
html_show_sphinx   = False
html_show_sourcelink = True

# ---------------------------------------------------------------------------
# Intersphinx — link to external projects' docs
# ---------------------------------------------------------------------------
intersphinx_mapping = {
    "python": ("https://docs.python.org/3", None),
    "numpy":  ("https://numpy.org/doc/stable/", None),
    "scipy":  ("https://docs.scipy.org/doc/scipy/", None),
    "pandas": ("https://pandas.pydata.org/docs/", None),
}

# ---------------------------------------------------------------------------
# General
# ---------------------------------------------------------------------------
exclude_patterns  = ["_build", ".venv", "Thumbs.db", ".DS_Store"]
source_suffix     = {".rst": "restructuredtext", ".md": "markdown"}
master_doc        = "index"
language          = "en"
pygments_style    = "monokai"
pygments_dark_style = "monokai"

# autosectionlabel — prefix with document name to avoid clashes
autosectionlabel_prefix_document = True

# copybutton — skip prompt characters in shell examples
copybutton_prompt_text = r"^\$ |^>>> |^In \[\d+\]: "
copybutton_prompt_is_regexp = True
