#!/usr/bin/env bash
# Build MOSAIC documentation (Doxygen + Sphinx).
# Usage:
#   ./scripts/build_docs.sh              → uses build/Debug for Doxygen XML
#   ./scripts/build_docs.sh Release      → uses build/Release
#
# Prerequisites:
#   doxygen    (brew install doxygen)
#   pip install -r docs/requirements.txt

set -euo pipefail

BUILD_TYPE="${1:-Debug}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build/$BUILD_TYPE"
DOXYGEN_XML="$BUILD_DIR/doxygen/xml"
SPHINX_OUT="$BUILD_DIR/docs/sphinx/html"

# ── 1. Doxygen ─────────────────────────────────────────────────────────────
echo "==> Running Doxygen..."
cmake -S "$ROOT" -B "$BUILD_DIR" -DMOSAIC_BUILD_DOCS=ON -DMOSAIC_BUILD_TESTS=OFF -q 2>/dev/null || true
cmake --build "$BUILD_DIR" --target docs_doxygen

echo "    XML output: $DOXYGEN_XML"

# ── 2. Sphinx ──────────────────────────────────────────────────────────────
echo "==> Running Sphinx..."
DOXYGEN_XML="$DOXYGEN_XML" \
    python3 -m sphinx -b html -E "$ROOT/docs" "$SPHINX_OUT"

echo ""
echo "==> Documentation built successfully."
echo "    Open: $SPHINX_OUT/index.html"

# macOS: auto-open in browser
if [[ "$(uname)" == "Darwin" ]]; then
    open "$SPHINX_OUT/index.html"
fi
