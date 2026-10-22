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
CMAKE_VER=$(sed -n 's/^project([^ ]* VERSION \([^ ]*\).*/\1/p' CMakeLists.txt | head -1)
# build_deb.sh now auto-derives version from CMakeLists.txt via sed
DEB_DERIVED="$CMAKE_VER"
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
QT_VER=$(sed -n "s/.*version:[[:space:]]*'\([^']*\)'.*/\1/p" .github/workflows/release.yml | head -1)
echo "  release.yml Qt version: $QT_VER"
if [ -n "$QT_VER" ] && [ "$(echo "$QT_VER" | cut -d. -f2)" -ge 8 ]; then
    ok "Qt version $QT_VER is recent enough for aqt XML parsing"
else
    warn "Qt version appears old — aqt may fail to find packages"
fi

# ── 6. Build Linux (quick) ──────────────────────────────────────────
echo "--- Build Linux ---"
if [ -f build/pengy ] && [ -f build/pengy-cli ] && [ -f build/pengy-web ]; then
    ok "Binaries already built (skipping rebuild)"
else
    echo "  Building..."
    ./build_linux.sh > /tmp/pengycpp_build.log 2>&1
    if [ -f build/pengy ] && [ -f build/pengy-cli ] && [ -f build/pengy-web ]; then
        ok "Build succeeded"
    else
        fail "Build failed — check /tmp/pengycpp_build.log"
    fi
fi

# ── 6b. Generated version.h — catches a stale build/ ────────────────
# "Binaries already built" above skips the rebuild, so a dirty build/ from an
# older tree can look fine while reporting the old version.
echo "--- Generated version.h ---"
if [ -f build/version.h ]; then
    VH=$(sed -n 's/^#define PENGY_VERSION "\(.*\)"/\1/p' build/version.h | head -1)
    echo "  build/version.h: $VH   (CMakeLists: $CMAKE_VER)"
    if [ "$VH" = "$CMAKE_VER" ]; then
        ok "version.h matches CMakeLists.txt"
    else
        fail "build/version.h says $VH but CMakeLists.txt says $CMAKE_VER — stale build dir: rm -rf build && ./build_linux.sh"
    fi
else
    warn "build/version.h missing"
fi

# ── 6c. GNU readline detection ─────────────────────────────────────
# v1.12.4's macOS DMG failed because the signal-aware prompt's hooks are GNU
# readline only. CMake decides by probing the symbol; if the probe fails here the
# CLI silently loses the signal-aware prompt, so say so out loud.
echo "--- GNU readline detection ---"
if [ -f build/CMakeCache.txt ]; then
    if grep -q 'PENGY_HAVE_RL_CATCH_SIGNALS:INTERNAL=1' build/CMakeCache.txt; then
        ok "GNU readline found (PENGY_GNU_READLINE on — signal-aware prompts enabled)"
    else
        warn "GNU readline NOT detected — CLI falls back to readline's own prompt handling (its prompts wait for Enter; SIGTERM/SIGHUP still reach the watcher). Install libreadline-dev"
    fi
else
    warn "build/CMakeCache.txt missing — cannot check readline detection"
fi

# ── 7. Verify binaries (existence + --version/--help smoke test) ───
echo "--- Verify binaries ---"
ls -lh build/pengy build/pengy-cli build/pengy-web build/pengy_tests 2>/dev/null
if command -v file >/dev/null 2>&1; then
    file build/pengy | grep -q "ELF" && ok "pengy is a valid ELF binary" || warn "pengy doesn't look like an ELF"
fi
for bin in build/pengy build/pengy-cli build/pengy-web; do
    name=$(basename "$bin")
    if [ ! -f "$bin" ]; then warn "$name not found"; continue; fi
    if "$bin" --version 2>/dev/null | grep -q "^Pengy v"; then
        ok "$name --version works"
    else
        warn "$name --version failed (stale binary?)"
    fi
    if "$bin" --help 2>/dev/null | grep -qiE "usage|options"; then
        ok "$name --help works"
    else
        warn "$name --help failed (stale binary?)"
    fi
done

# ── 7b. Test suite (QTest, headless) ───────────────────────────────
# The suite never ran in this pre-flight before; it takes ~80 s here.
echo "--- Test suite ---"
if [ -x build/pengy_tests ]; then
    if QT_QPA_PLATFORM=offscreen timeout 900 ./build/pengy_tests > /tmp/pengycpp_tests.log 2>&1; then
        ok "pengy_tests: $(grep -o 'Totals: .*' /tmp/pengycpp_tests.log | head -1)"
    else
        warn "pengy_tests failed — check /tmp/pengycpp_tests.log"
        grep -E "^FAIL|Totals:" /tmp/pengycpp_tests.log | head -10
    fi
else
    warn "build/pengy_tests not found — skipping the suite (build_linux.sh builds it)"
fi

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
