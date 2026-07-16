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

# ── Smoke test: verify --version and --help on every binary ─────────
echo "==> Smoke testing binaries..."
PASS=0; FAIL=0
smoke() {
    local bin="$1" name; name=$(basename "$bin")
    if [ ! -f "$bin" ]; then
        echo -e "  \033[31m✗\033[0m $name not found"
        FAIL=$((FAIL+1)); return
    fi
    if "$bin" --version 2>/dev/null | grep -q "^Pengy v" && \
       "$bin" --help    2>/dev/null | grep -qiE "usage|options"; then
        echo -e "  \033[32m✓\033[0m $name --version + --help"
        PASS=$((PASS+1))
    else
        echo -e "  \033[31m✗\033[0m $name --version/--help failed (stale or broken?)"
        FAIL=$((FAIL+1))
    fi
}
smoke "$ROOT/build/pengy"
smoke "$ROOT/build/pengy_cli"
smoke "$ROOT/build/pengy_web"
if [ "$FAIL" -gt 0 ]; then
    echo -e "\033[31m==> $FAIL binary(s) failed smoke test!\033[0m"
    exit 1
fi
echo "==> All $PASS binary(s) passed smoke test."
