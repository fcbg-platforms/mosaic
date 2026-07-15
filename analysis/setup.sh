#!/usr/bin/env bash
# MOSAIC analysis environment setup — macOS / Linux
# Usage: bash analysis/setup.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"

echo "==> Creating Python virtual environment at $VENV_DIR"
python3 -m venv "$VENV_DIR"

source "$VENV_DIR/bin/activate"

echo "==> Upgrading pip"
pip install --upgrade pip wheel

echo "==> Installing analysis dependencies"
pip install -r "$SCRIPT_DIR/requirements.txt"

echo ""
echo "==> Setup complete.  To activate the environment:"
echo "    source analysis/.venv/bin/activate"
echo ""
echo "==> To run pose estimation on a recorded session:"
echo "    python analysis/run_pose.py --session /path/to/session"
echo ""
echo "==> YOLOv8 model weights (~6 MB) will be downloaded on first run."
