#!/usr/bin/env bash
# Configure MOSAIC for macOS (Apple Silicon or Intel).
# Usage:
#   ./scripts/configure.sh                         → Debug build
#   ./scripts/configure.sh Release                 → Release build
#   ./scripts/configure.sh Debug --tests           → Debug + unit tests
#   ./scripts/configure.sh Debug --opencv          → Debug + calibration

set -euo pipefail

BUILD_TYPE="${1:-Debug}"
BUILD_TESTS=OFF
ENABLE_OPENCV=OFF
ENABLE_FFMPEG=OFF
for arg in "$@"; do
    [[ "$arg" == "--tests" ]]  && BUILD_TESTS=ON
    [[ "$arg" == "--opencv" ]] && ENABLE_OPENCV=ON
    [[ "$arg" == "--ffmpeg" ]] && ENABLE_FFMPEG=ON
done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build/$BUILD_TYPE"

# ── Locate Qt 6 ──────────────────────────────────────────────────────────
find_qt() {
    # 1. Homebrew (Apple Silicon: /opt/homebrew, Intel: /usr/local)
    for prefix in /opt/homebrew /usr/local; do
        local qt_dir="$prefix/opt/qt"
        if [[ -d "$qt_dir/lib/cmake/Qt6" ]]; then
            echo "$qt_dir"; return
        fi
    done
    # 2. Qt installer default locations
    for qt_root in "$HOME/Qt" "/Applications/Qt"; do
        if [[ -d "$qt_root" ]]; then
            # Pick the highest 6.x version available
            local best
            best=$(find "$qt_root" -maxdepth 2 -name "Qt6Config.cmake" 2>/dev/null \
                   | grep "6\." | sort -rV | head -1)
            if [[ -n "$best" ]]; then
                echo "$(dirname "$(dirname "$(dirname "$best")")")"; return
            fi
        fi
    done
    echo ""
}

QT_PREFIX=$(find_qt)
if [[ -z "$QT_PREFIX" ]]; then
    echo "ERROR: Qt 6 not found."
    echo "  Install via Homebrew:  brew install qt"
    echo "  Or download from:      https://www.qt.io/download"
    exit 1
fi
echo "Using Qt at: $QT_PREFIX"

# ── Configure ─────────────────────────────────────────────────────────────
cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMOSAIC_BUILD_TESTS="$BUILD_TESTS" \
    -DMOSAIC_ENABLE_CAMERAS=OFF \
    -DMOSAIC_ENABLE_NVENC=OFF \
    -DMOSAIC_ENABLE_FFMPEG="$ENABLE_FFMPEG" \
    -DMOSAIC_ENABLE_PARALLEL_PORT=OFF \
    -DMOSAIC_ENABLE_OPENCV="$ENABLE_OPENCV"

# Symlink compile_commands.json to the root so clangd / VS Code can find it.
ln -sf "$BUILD_DIR/compile_commands.json" "$ROOT/compile_commands.json" 2>/dev/null || true

echo ""
echo "Configured. Now build with:"
echo "  cmake --build $BUILD_DIR --parallel"
echo ""
echo "Then run:"
echo "  $BUILD_DIR/bin/mosaic"
