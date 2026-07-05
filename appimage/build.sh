#!/bin/bash
# Build Pengy AppImage
# Requires: linuxdeploy + linuxdeploy-plugin-qt in appimage/tools/
#   wget -P appimage/tools https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
#   wget -P appimage/tools https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
#   chmod +x appimage/tools/*.AppImage
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APPIMAGE_DIR="$ROOT"
TOOLS="$APPIMAGE_DIR/tools"
APPDIR="$APPIMAGE_DIR/Pengy.AppDir"
PROJECT_ROOT="$(dirname "$ROOT")"

echo "==> Cleaning AppDir..."
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
         "$APPDIR/usr/share/applications" "$APPDIR/usr/plugins/platforms" \
         "$APPDIR/usr/lib"

# 1. Build
echo "==> Building Pengy..."
cd "$PROJECT_ROOT"
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 2. Copy binary + assets to AppDir
echo "==> Assembling AppDir..."
cp "$PROJECT_ROOT/build/pengy" "$APPDIR/usr/bin/"
cp "$APPIMAGE_DIR/pengy.desktop" "$APPDIR/usr/share/applications/"
# Resize icon to 256x256 for linuxdeploy (which validates dimensions match path)
if command -v convert &>/dev/null; then
    convert "$PROJECT_ROOT/pengy.png" -resize 256x256 "$APPDIR/usr/share/icons/hicolor/256x256/apps/pengy.png"
    cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/pengy.png" "$APPDIR/pengy.png"
else
    echo "WARNING: ImageMagick not found, icon may fail validation"
    cp "$PROJECT_ROOT/pengy.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/pengy.png"
    cp "$PROJECT_ROOT/pengy.png" "$APPDIR/pengy.png"
fi

# 3. Copy Wayland platform plugin + libs (linuxdeploy-plugin-qt only
#    bundles XCB by default; without wayland the AppImage falls back
#    to XWayland and looks blurry on HiDPI)
echo "==> Bundling Wayland plugin..."
QT6_PLUGINS="/usr/lib/x86_64-linux-gnu/qt6/plugins"
if [ -f "$QT6_PLUGINS/platforms/libqwayland.so" ]; then
    cp "$QT6_PLUGINS/platforms/libqwayland.so" "$APPDIR/usr/plugins/platforms/"
    # dependencies that linuxdeploy may miss
    for lib in libQt6WaylandClient.so.6 libwayland-client.so.0 \
               libwayland-cursor.so.0 libxkbcommon.so.0; do
        if [ -f "/usr/lib/x86_64-linux-gnu/$lib" ]; then
            cp "/usr/lib/x86_64-linux-gnu/$lib" "$APPDIR/usr/lib/"
        fi
    done
    # Copy Wayland shell-integration plugins (xdg-shell etc).
    # Without these, Qt prints "No shell integration named 'xdg-shell' found"
    # and falls back to XWayland (blurry on HiDPI).
    if [ -d "$QT6_PLUGINS/wayland-shell-integration" ]; then
        mkdir -p "$APPDIR/usr/plugins/wayland-shell-integration"
        cp -a "$QT6_PLUGINS/wayland-shell-integration/"* "$APPDIR/usr/plugins/wayland-shell-integration/"
    fi
    if [ -d "$QT6_PLUGINS/wayland-graphics-integration-client" ]; then
        mkdir -p "$APPDIR/usr/plugins/wayland-graphics-integration-client"
        cp -a "$QT6_PLUGINS/wayland-graphics-integration-client/"* "$APPDIR/usr/plugins/wayland-graphics-integration-client/"
    fi
    if [ -d "$QT6_PLUGINS/wayland-decoration-client" ]; then
        mkdir -p "$APPDIR/usr/plugins/wayland-decoration-client"
        cp -a "$QT6_PLUGINS/wayland-decoration-client/"* "$APPDIR/usr/plugins/wayland-decoration-client/"
    fi
else
    echo "WARNING: Wayland plugin not found, AppImage will use X11 only"
fi

# 4. Run linuxdeploy with Qt plugin
echo "==> Bundling with linuxdeploy..."
export QML_SOURCES_PATHS="$PROJECT_ROOT"
export LDAI_OUTPUT="$PROJECT_ROOT/Pengy-x86_64.AppImage"

"$TOOLS/linuxdeploy-x86_64.AppImage" \
    --appdir "$APPDIR" \
    --plugin qt \
    --desktop-file "$APPDIR/usr/share/applications/pengy.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/pengy.png" \
    --output appimage 2>&1

echo ""
echo "==> Done!"
ls -lh "$PROJECT_ROOT/Pengy-x86_64.AppImage" 2>/dev/null || echo "AppImage not found, check output above"
