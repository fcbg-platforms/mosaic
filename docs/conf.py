"""
Sphinx configuration for MOSAIC documentation.

Build pipeline:
    1. CMake (or ``doxygen docs/Doxyfile.in``) produces Doxygen XML.
    2. Breathe reads the XML and exposes C++ entities to Sphinx.
    3. Exhale walks the Breathe tree and auto-generates RST API pages.
    4. Sphinx renders everything to HTML (or PDF via LaTeX).

Quickstart::

    pip install -r docs/requirements.txt
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
    "myst_parser",          # Markdown support
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
html_theme = "pydata-sphinx-theme"

html_title       = "MOSAIC"
html_short_title = "MOSAIC"

html_theme_options = {
    "logo": {
        "text": "MOSAIC",
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
html_favicon       = None
html_show_sphinx   = False
html_show_sourcelink = True

# ---------------------------------------------------------------------------
# Intersphinx — link to external projects' docs
# ---------------------------------------------------------------------------
intersphinx_mapping = {
    "python": ("https://docs.python.org/3", None),
}

# ---------------------------------------------------------------------------
# General
# ---------------------------------------------------------------------------
exclude_patterns  = ["_build", "Thumbs.db", ".DS_Store"]
source_suffix     = {".rst": "restructuredtext", ".md": "myst_parser"}
master_doc        = "index"
language          = "en"
pygments_style    = "monokai"
pygments_dark_style = "monokai"

# autosectionlabel — prefix with document name to avoid clashes
autosectionlabel_prefix_document = True

# copybutton — skip prompt characters in shell examples
copybutton_prompt_text = r"^\$ |^>>> |^In \[\d+\]: "
copybutton_prompt_is_regexp = True
