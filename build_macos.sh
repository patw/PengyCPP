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
echo "==> Done! Binary: pengy (in build_macos/)"

echo "==> Creating Pengy.app bundle..."
APP_DIR="$ROOT/Pengy.app"
mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"
cp pengy "$APP_DIR/Contents/MacOS/"
cp "$ROOT/pengy.png" "$APP_DIR/Contents/Resources/"
# Create an icns if the png is available (macOS prefers icns for dock/titlebar)
if command -v sips &>/dev/null; then
    PNG="$ROOT/pengy.png"
    ICONSET_DIR="$APP_DIR/Contents/Resources/Pengy.iconset"
    mkdir -p "$ICONSET_DIR"
    for size in 16 32 64 128 256 512; do
        sips -z "$size" "$size" "$PNG" --out "$ICONSET_DIR/icon_${size}x${size}.png" &>/dev/null
        sips -z $((size*2)) $((size*2)) "$PNG" --out "$ICONSET_DIR/icon_${size}x${size}@2x.png" &>/dev/null
    done
    iconutil -c icns "$ICONSET_DIR" -o "$APP_DIR/Contents/Resources/Pengy.icns" 2>/dev/null && \
        rm -rf "$ICONSET_DIR"
    # Set the icon in Info.plist
    /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string Pengy" "$APP_DIR/Contents/Info.plist" 2>/dev/null || \
        /usr/libexec/PlistBuddy -c "Set :CFBundleIconFile Pengy" "$APP_DIR/Contents/Info.plist"
fi
macdeployqt "$APP_DIR" -verbose=2

# macdeployqt modifies dylib load paths which invalidates existing signatures;
# re-sign everything with an ad-hoc signature so macOS will launch the app
codesign --force --deep --sign - "$APP_DIR"

echo "==> App bundle: $APP_DIR"

# Create DMG for distribution
echo "==> Creating DMG..."
DMG_NAME="Pengy-macOS-$ARCH.dmg"
DMG_STAGING="$ROOT/.dmg_staging"
rm -rf "$DMG_STAGING"
mkdir -p "$DMG_STAGING"
cp -r "$APP_DIR" "$DMG_STAGING/"
ln -s /Applications "$DMG_STAGING/Applications"

# Volume icon (reuse the icns already placed in the bundle)
cp "$APP_DIR/Contents/Resources/Pengy.icns" "$DMG_STAGING/.VolumeIcon.icns"

hdiutil create \
    -volname "Pengy" \
    -srcfolder "$DMG_STAGING" \
    -ov \
    -format UDRW \
    "$ROOT/$DMG_NAME"

# Set the volume's custom icon bit so Finder uses .VolumeIcon.icns
DMG_MOUNT=$(hdiutil attach -readwrite -noverify "$ROOT/$DMG_NAME" | grep -oE '/Volumes/.+$')
if command -v SetFile &>/dev/null; then
    SetFile -a C "$DMG_MOUNT"
fi
hdiutil detach "$DMG_MOUNT" -quiet

# Convert to compressed read-only DMG
hdiutil convert "$ROOT/$DMG_NAME" -format UDZO -o "$ROOT/${DMG_NAME%.dmg}-compressed.dmg" -ov
mv "$ROOT/${DMG_NAME%.dmg}-compressed.dmg" "$ROOT/$DMG_NAME"

rm -rf "$DMG_STAGING"
echo "==> DMG ready: $ROOT/$DMG_NAME"
