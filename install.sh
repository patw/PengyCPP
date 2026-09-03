#!/bin/bash
# Install Pengy (C++/Qt6) binaries to ~/.local/bin/
#   ./install.sh              # build (cmake + make) + install
#   ./install.sh --prebuilt   # install from existing build/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="${INSTALL_DIR:-$HOME/.local/bin}"

# macOS does not provide nproc; use an override, getconf, or sysctl.
if [[ -n "${JOBS:-}" ]]; then
    BUILD_JOBS="$JOBS"
elif command -v nproc >/dev/null 2>&1; then
    BUILD_JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    BUILD_JOBS="$(sysctl -n hw.ncpu)"
else
    BUILD_JOBS=4
fi

# Build (unless --prebuilt): same flow as build_linux.sh
if [[ "${1:-}" != "--prebuilt" ]]; then
    echo "==> Building Pengy (C++/Qt6) release..."
    cd "$ROOT"
    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j"$BUILD_JOBS"
fi

echo ""
echo "==> Installing to $INSTALL_DIR..."
mkdir -p "$INSTALL_DIR"

for bin in pengy pengy-cli pengy-web; do
    src="$ROOT/build/$bin"
    if [[ ! -f "$src" ]]; then
        echo "ERROR: $src not found. Build first with: ./install.sh  (or --prebuilt if already built)"
        exit 1
    fi
    cp "$src" "$INSTALL_DIR/$bin"
    chmod +x "$INSTALL_DIR/$bin"
    echo "    $INSTALL_DIR/$bin"
done

# Check if INSTALL_DIR is in PATH
if ! echo "$PATH" | tr ':' '\n' | grep -qx "$INSTALL_DIR"; then
    echo ""
    echo "NOTE: $INSTALL_DIR is not in your PATH."
    echo "Add it with:  export PATH=\"$INSTALL_DIR:\$PATH\""
    echo "Or add that line to your ~/.bashrc or ~/.zshrc"
fi

echo ""
echo "==> Done! Installed:"
echo "    pengy      — desktop GUI"
echo "    pengy-cli  — interactive REPL or single-shot: pengy-cli \"question\""
echo "    pengy-web  — web UI: pengy-web [port]  (default: 5000)"
