#!/bin/bash
# Pre-flight checks — run before `git push --tags`
# Catches the CI failures we've hit before so you don't have to wait 3 min × N iterations.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

WARNINGS=0
warn() { echo -e "\033[33m  WARNING: $*\033[0m"; WARNINGS=$((WARNINGS + 1)); }
fail() { echo -e "\033[31m  FAIL: $*\033[0m"; exit 1; }
ok()   { echo -e "\033[32m  ✓ $*\033[0m"; }

echo "========================================="
echo " PengyCPP Pre-Flight Release Check"
echo "========================================="

# ── 1. Version consistency ──────────────────────────────────────────
echo "--- Version consistency ---"
CMAKE_VER=$(grep -oP 'project\(\w+ VERSION \K[\d.]+' CMakeLists.txt | head -1)
# build_deb.sh now auto-derives version from CMakeLists.txt via grep
DEB_DERIVED=$(grep -oP 'project\(\w+ VERSION \K[\d.]+' CMakeLists.txt | head -1)
echo "  CMakeLists.txt:  $CMAKE_VER"
echo "  build_deb.sh:    auto-detected from CMakeLists.txt → $DEB_DERIVED"
if [ -n "$CMAKE_VER" ]; then
    ok "Version: $CMAKE_VER (single source of truth in CMakeLists.txt)"
else
    warn "Could not parse version from CMakeLists.txt"
fi

# ── 2. Icon dimensions (linuxdeploy validates this strictly) ────────
echo "--- Icon dimensions ---"
if command -v identify &>/dev/null; then
    ICON_DIMS=$(identify -format '%w %h' pengy.png 2>/dev/null)
    ICON_W=$(echo "$ICON_DIMS" | cut -d' ' -f1)
    ICON_H=$(echo "$ICON_DIMS" | cut -d' ' -f2)
    echo "  pengy.png: ${ICON_W}x${ICON_H}"
    if [ "$ICON_W" = "$ICON_H" ] && [ "$ICON_W" -eq 256 ]; then
        ok "Icon is 256x256 (linuxdeploy-compatible)"
    else
        warn "Icon is ${ICON_W}x${ICON_H}, linuxdeploy requires exactly 256x256 for AppImage"
        warn "  AppImage build.sh resizes via ImageMagick in CI, but local build may fail"
    fi
else
    warn "ImageMagick 'identify' not installed — can't check icon dimensions"
fi

# ── 3. macOS path sanity ────────────────────────────────────────────
echo "--- macOS build script ---"
if grep -q 'cp build_macos/pengy' build_macos.sh 2>/dev/null; then
    fail "build_macos.sh has 'cp build_macos/pengy' — should be 'cp pengy' (after cd build_macos)"
else
    ok "build_macos.sh path looks correct"
fi

# ── 4. CI release.yml permissions ───────────────────────────────────
echo "--- Release workflow ---"
if grep -q 'contents: write' .github/workflows/release.yml; then
    ok "release.yml has 'contents: write' permission"
else
    warn "release.yml missing 'contents: write' — upload step will fail with 403"
fi

# ── 5. CI release.yml Windows Qt version ────────────────────────────
echo "--- Windows Qt version in CI ---"
QT_VER=$(grep -oP "version:\s*'[^']+'" .github/workflows/release.yml | head -1 | grep -oP "[\d.]+")
echo "  release.yml Qt version: $QT_VER"
if [ -n "$QT_VER" ] && [ "$(echo "$QT_VER" | cut -d. -f2)" -ge 8 ]; then
    ok "Qt version $QT_VER is recent enough for aqt XML parsing"
else
    warn "Qt version appears old — aqt may fail to find packages"
fi

# ── 6. Build Linux (quick) ──────────────────────────────────────────
echo "--- Build Linux ---"
if [ -f build/pengy ] && [ -f build/pengy_cli ] && [ -f build/pengy_web ]; then
    ok "Binaries already built (skipping rebuild)"
else
    echo "  Building..."
    ./build_linux.sh > /tmp/pengycpp_build.log 2>&1
    if [ -f build/pengy ] && [ -f build/pengy_cli ] && [ -f build/pengy_web ]; then
        ok "Build succeeded"
    else
        fail "Build failed — check /tmp/pengycpp_build.log"
    fi
fi

# ── 7. Verify binaries ──────────────────────────────────────────────
echo "--- Verify binaries ---"
ls -lh build/pengy build/pengy_cli build/pengy_web build/pengy_tests 2>/dev/null
file build/pengy | grep -q "ELF" && ok "pengy is a valid ELF binary" || warn "pengy doesn't look like an ELF"

# ── 8. .deb package ─────────────────────────────────────────────────
echo "--- .deb package ---"
if command -v dpkg-deb &>/dev/null; then
    ./build_deb.sh > /tmp/pengycpp_deb.log 2>&1
    DEB_FILE=$(ls -1t pengy_*.deb 2>/dev/null | head -1)
    if [ -f "$DEB_FILE" ]; then
        ok ".deb created: $(ls -lh "$DEB_FILE" | awk '{print $5, $NF}')"
        rm -f "$DEB_FILE"
    else
        warn ".deb build failed — check /tmp/pengycpp_deb.log"
    fi
else
    warn "dpkg-deb not available — skipping .deb check"
fi

# ── Summary ─────────────────────────────────────────────────────────
echo ""
echo "========================================="
if [ $WARNINGS -eq 0 ]; then
    echo -e "\033[32m All checks passed! Ready to tag.\033[0m"
else
    echo -e "\033[33m $WARNINGS warning(s) found — review above before tagging.\033[0m"
fi
echo "========================================="
