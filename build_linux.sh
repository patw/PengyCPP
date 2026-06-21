#!/bin/bash
# Build Pengy for Linux
# Prerequisites (Ubuntu/Debian):
#   sudo apt install build-essential cmake qt6-base-dev libgl-dev
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

echo "==> Building Pengy (C++/Qt6) for Linux..."
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

echo ""
echo "==> Done! Binary: build/pengy"
echo "==> Run with: ./build/pengy"
