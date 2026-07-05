# PengyCPP 🐧

**PengyCPP** is a pure C++17/Qt6 rewrite of [Pengy](https://github.com/patw/Pengy) — a local-first AI agent application that connects to any OpenAI-compatible LLM API and gives the model tools to operate on your machine.

> **Beta** — PengyCPP is a C++ port of the Python Pengy. Chat history and settings are fully interoperable between all versions (all use `~/.config/pengy/`), but may be missing some features compared to the Python version.

```
PengyCPP/
├── CMakeLists.txt          # Single CMake project, no Rust; builds pengy, pengy_cli, pengy_web
├── main.cpp                # Desktop GUI entry point
├── config.cpp/h            # Settings: ~/.config/pengy/settings.json
├── chatmanager.cpp/h       # Chats: ~/.config/pengy/chats.json
├── tools.cpp/h             # 11 OpenAI function-calling tools
├── llmclient.cpp/h         # Blocking LLM chat loop (QNetworkAccessManager)
├── chatworker.cpp/h        # QThread worker + QWaitCondition confirmation
├── mainwindow.cpp/h        # Three-pane main window
├── chathistory.cpp/h       # Sidebar — chat list with 💾/🗑 buttons
├── chatview.cpp/h          # Chat display — markdown, tables, collapsible tool blocks
├── chatinput.cpp/h         # Message input
├── settingsdialog.cpp/h    # Settings dialog + Fetch Models
├── cli/
│   └── main.cpp            # Interactive REPL + single-shot mode
├── web/
│   ├── main.cpp            # Web server entry point (default port 5000)
│   ├── webserver.cpp/h     # QTcpServer HTTP + SSE server
│   ├── webchatworker.cpp/h # QThread worker for web (mirrors chatworker)
│   ├── web_resources.qrc   # Embeds HTML templates into binary
│   └── templates/
│       ├── chat.html       # Bootstrap 5 chat UI with SSE
│       └── settings.html   # Settings form
├── tests.cpp               # Qt Test suite
├── appimage/
│   ├── build.sh            # Bundles Pengy-x86_64.AppImage
│   ├── pengy.desktop       # Desktop entry
│   └── tools/              # Place linuxdeploy + linuxdeploy-plugin-qt here
├── build_linux.sh          # Linux native build
├── build_macos.sh          # macOS build + Pengy.app + DMG
├── build_windows.bat       # Windows build (MSVC Qt6)
├── build_deb.sh            # Debian/Ubuntu .deb package
└── SPEC.md                 # Full architecture specification
```

## Quick Start

### Linux

```bash
# Dependencies (Ubuntu/Debian):
sudo apt install build-essential cmake qt6-base-dev libgl-dev

./build_linux.sh
./build/pengy          # desktop GUI
./build/pengy_cli      # terminal REPL
./build/pengy_web      # web UI at http://localhost:5000
```

### Linux AppImage (portable, no system deps)

```bash
# Download linuxdeploy tools first (one time):
wget -P appimage/tools https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
wget -P appimage/tools https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
chmod +x appimage/tools/*.AppImage

cd appimage && ./build.sh
# → Pengy-x86_64.AppImage
```

### Linux .deb package

```bash
./build_deb.sh
# → pengy_1.2.4_amd64.deb
sudo dpkg -i pengy_1.2.4_amd64.deb
```

### macOS

```bash
brew install qt@6 cmake
./build_macos.sh [arm64|x86_64]
# → Pengy.app
# → Pengy-macOS-[arch].dmg
```

### Windows

```
REM Prerequisites: Qt6 (MSVC 64-bit), VS Build Tools 2022, CMake
REM Run from a VS 2022 Developer Command Prompt:
build_windows.bat
REM → Pengy-Windows\pengy.exe  (Qt DLLs bundled)
```

## Architecture

| Layer | What |
|-------|------|
| `config` | Load/save `~/.config/pengy/settings.json`; render system message templates |
| `chatmanager` | Chat CRUD, `~/.config/pengy/chats.json`, message cleaning |
| `tools` | 11 tools using `QFile`, `QProcess`, `QNetworkAccessManager`, `QDirIterator` |
| `llmclient` | Blocking OpenAI-compatible chat loop; tool call dispatch |
| `chatworker` | Runs `LlmClient::run()` on a `QThread`; `QWaitCondition` for tool confirmation |
| `webchatworker` | Same as chatworker but for `pengy_web`; emits SSE events via Qt signals |
| `webserver` | `QTcpServer` HTTP server with SSE push; Bootstrap 5 UI from Qt Resources |
| `mainwindow` | Three-pane window; tool confirmation modal; wires all signals/slots |
| `chathistory` | Sidebar with per-row 💾 (export to Markdown) and 🗑 (delete) buttons |
| `chatview` | `QTextBrowser` with custom markdown→HTML pipeline |

Three binaries sharing the same config and chat storage. No runtime dependencies beyond system Qt6.

## Feature Parity

| Feature | Python Pengy | PengyCPP |
|---------|:---:|:---:|
| OpenAI-compatible LLM API | ✅ | ✅ |
| 11 tools (bash, python, files, web, etc.) | ✅ | ✅ |
| Three-pane Qt6 desktop GUI | ✅ | ✅ |
| Markdown + table rendering | ✅ | ✅ |
| Collapsible tool call blocks | ✅ | ✅ |
| Chat sidebar with 💾/🗑 buttons | ✅ | ✅ |
| Settings dialog + Fetch Models | ✅ | ✅ |
| Tool confirmation (YOLO / Safe / None) | ✅ | ✅ |
| Sudo password support | ✅ | ✅ |
| System message templates ({date} etc.) | ✅ | ✅ |
| File attachments (`@/path/to/file`) | ✅ | ✅ |
| CLI REPL + single-shot mode | ✅ | ✅ |
| Web UI (Bootstrap 5, SSE streaming) | ✅ | ✅ |

## Interoperability

PengyCPP shares `~/.config/pengy/` with the Python and Rust versions:
- **`settings.json`** — Same format, all versions read/write it
- **`chats.json`** — Same message schema. Chats created in any version can be loaded in any other

## Development

```bash
# Build all three binaries
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)

# Run tests
cmake --build build --target pengy_tests && ./build/pengy_tests

# Run CLI
./build/pengy_cli                     # interactive REPL
./build/pengy_cli "what is 2+2"      # single-shot

# Run web UI
./build/pengy_web                     # http://localhost:5000
./build/pengy_web 8080               # custom port

# Format code
clang-format -i *.cpp *.h cli/*.cpp web/*.cpp web/*.h
```

## License

MIT
