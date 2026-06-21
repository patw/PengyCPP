#!/bin/bash
# Build Pengy for macOS
# Prerequisites:
#   brew install qt@6 cmake
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
ARCH="${1:-$(uname -m)}"  # arm64 or x86_64

export CMAKE_PREFIX_PATH="$(brew --prefix qt@6 2>/dev/null || echo '/opt/homebrew/opt/qt@6')"
export PATH="$CMAKE_PREFIX_PATH/bin:$PATH"

echo "==> Building Pengy (C++/Qt6) for macOS ($ARCH)..."
cd "$ROOT"
mkdir -p build_macos
cd build_macos

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH"

make -j$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo ""
echo "==> Done! Binary: build_macos/pengy"

echo "==> Creating Pengy.app bundle..."
APP_DIR="$ROOT/Pengy.app"
mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"
cp build_macos/pengy "$APP_DIR/Contents/MacOS/"
macdeployqt "$APP_DIR" -verbose=2

echo "==> App bundle: $APP_DIR"
echo "==> Distribute by zipping: zip -r Pengy-macOS-$ARCH.zip Pengy.app"
