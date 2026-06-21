# Pengy (C++ edition) 🐧

A pure C++17/Qt6 local AI agent desktop application. Connects to any OpenAI-compatible LLM API and gives the model tools to operate on your machine.

This is the third generation of the Pengy family — a rewrite of [PengyR](../PengyR) (Rust core + Qt6 GUI) with all logic moved to C++, eliminating the Rust toolchain dependency.

```
PengyCPP/
├── CMakeLists.txt          # Single CMake project, no Rust
├── main.cpp
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
├── build_linux.sh
├── build_macos.sh
└── build_windows.bat
```

## Quick Start

### Linux

```bash
# Dependencies (Ubuntu/Debian):
sudo apt install build-essential cmake qt6-base-dev libgl-dev

./build_linux.sh
./build/pengy
```

### macOS

```bash
brew install qt@6 cmake
./build_macos.sh [arm64|x86_64]
# → Pengy.app
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
| `mainwindow` | Three-pane window; tool confirmation modal; wires all signals/slots |
| `chathistory` | Sidebar with per-row 💾 (export to Markdown) and 🗑 (delete) buttons |
| `chatview` | `QTextBrowser` with custom markdown→HTML pipeline |

Single ~8 MB binary. No runtime dependencies beyond system Qt6.

## Feature Parity

| Feature | Python | PengyCPP |
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
| File attachments | ✅ | ❌ |
| CLI / Web UI | ✅ | ❌ |

## Interoperability

PengyCPP shares `~/.config/pengy/` with the Python and Rust versions. Settings and chat history created in any version can be opened in any other.

## License

MIT
