# Building from Source (C++)

## Prerequisites

- CMake ≥ 3.16
- C++17 compiler (GCC ≥ 8, Clang ≥ 7, MSVC 2019+)
- Qt 6.4+

**No Rust, no Python, no third-party C++ libraries.** Everything is Qt6 + STL.

## Linux

```bash
# Dependencies
sudo apt install build-essential cmake qt6-base-dev libgl-dev

# Build everything (GUI + CLI + Web)
./build_linux.sh

# GUI
./build/pengy

# CLI
./build/pengy_cli

# Web
./build/pengy_web
```

### AppImage

```bash
# Download linuxdeploy tools (one time):
wget -P appimage/tools https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
wget -P appimage/tools https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
chmod +x appimage/tools/*.AppImage

cd appimage && ./build.sh
# → Pengy-x86_64.AppImage
```

### .deb package

```bash
./build_deb.sh
# → pengy_<version>_amd64.deb
```

## macOS

```bash
brew install qt@6 cmake
./build_macos.sh [arm64|x86_64]
# → Pengy.app
# → Pengy-macOS-<arch>.dmg
```

## Windows

From a VS 2022 Developer Command Prompt:

```
REM Prerequisites: Qt6 (MSVC 64-bit), VS Build Tools 2022, CMake
build_windows.bat
REM → Pengy-Windows\pengy.exe  (Qt DLLs bundled)
```

## Running tests

```bash
cmake --build build --target pengy_tests
./build/pengy_tests
```

## Architecture notes

PengyCPP is a single CMake project producing three binaries (`pengy`, `pengy_cli`, `pengy_web`). All logic is pure C++17 — no FFI, no external dependencies beyond Qt6. The web UI uses a lightweight QTcpServer instead of an external HTTP framework.
