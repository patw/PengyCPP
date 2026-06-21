# Pengy — Application Specification

## Overview

Pengy (C++ edition) is a pure C++17/Qt6 desktop AI agent that connects to any OpenAI-compatible LLM API and gives the model a set of tools to operate on the user's machine. It is the third generation of the Pengy family:

| Version | Stack | Notes |
|---------|-------|-------|
| Pengy | Python + PyQt5 | Original |
| PengyR | Rust core + Qt6 C++ GUI | Rust → C FFI → Qt |
| **PengyCPP** | **Pure C++17 + Qt6** | **No Rust, no FFI** |

> PengyCPP shares `~/.config/pengy/` with the Python and Rust versions. Settings and chat history are fully interoperable across all three.

---

## Technology Stack

- **Language:** C++17 (single CMake project, no external runtimes)
- **GUI Framework:** Qt6 Widgets
- **HTTP:** `QNetworkAccessManager` + local `QEventLoop` (synchronous-style calls on worker threads)
- **JSON:** `QJsonDocument` / `QJsonObject` / `QJsonArray`
- **Threading:** `QThread::create()` lambda + `QMutex` / `QWaitCondition` for tool confirmation
- **Markdown Rendering:** `QTextBrowser` with custom regex pipeline
- **Storage:** JSON files in `~/.config/pengy/` (shared with Python and Rust versions)

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  Qt6 GUI + Application Logic (C++17, single binary)      │
│                                                          │
│  ┌─────────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │ ChatHistory │  │ ChatView │  │ ChatInput        │   │
│  │  (sidebar)  │  │  (QTB)   │  │ (QPlainTextEdit) │   │
│  └─────────────┘  └──────────┘  └──────────────────┘   │
│                                                          │
│  ┌──────────────────────────────────────────────────┐   │
│  │ MainWindow — orchestrates signals/slots          │   │
│  └──────────────────────────────────────────────────┘   │
│                                                          │
│  ┌──────────────────────────────────────────────────┐   │
│  │ ChatWorker (QThread) — runs LlmClient::run()     │   │
│  └──────────────────────────────────────────────────┘   │
│                                                          │
│  ┌──────────────────────────────────────────────────┐   │
│  │ LlmClient — syncPost() HTTP loop + tool calls    │   │
│  └──────────────────────────────────────────────────┘   │
│                                                          │
│  ┌──────────┐  ┌─────────────┐  ┌──────────────┐       │
│  │  Config  │  │  ChatMgr    │  │    Tools     │       │
│  │ config.* │  │ chatmanager*│  │   tools.*    │       │
│  └──────────┘  └─────────────┘  └──────────────┘       │
└──────────────────────────────────────────────────────────┘
```

### Source Layout

```
PengyCPP/
├── CMakeLists.txt          # Single CMake project, no Rust
├── main.cpp                # Entry point — QApplication + ui_scale env
├── config.cpp/h            # Settings load/save + system message rendering
├── chatmanager.cpp/h       # Chat session CRUD + message cleaning
├── tools.cpp/h             # 11 OpenAI function-calling tools (Qt APIs)
├── llmclient.cpp/h         # Blocking LLM chat loop (QNetworkAccessManager)
├── chatworker.cpp/h        # QThread worker — runs LlmClient + confirmation sync
├── mainwindow.cpp/h        # Three-pane main window, tool confirmation dialog
├── chathistory.cpp/h       # Left sidebar — chat list with 💾/🗑 buttons
├── chatview.cpp/h          # Chat display — markdown, tables, collapsible tool blocks
├── chatinput.cpp/h         # Message input (QPlainTextEdit)
├── settingsdialog.cpp/h    # Settings modal + Fetch Models button
├── build_linux.sh          # Linux build script
├── build_macos.sh          # macOS build script (Homebrew Qt6)
└── build_windows.bat       # Windows build script (MSVC Qt6)
```

---

## GUI Layout

```
┌────────────────────┬────────────────────────────────────────────────────┐
│  + New Chat        │                                                    │
│  ⚙ Settings        │           Chat View (Markdown)                     │
│                    │                                                    │
│  ─────────────     │  🧑 You                                            │
│  Chat 1  💾 🗑      │  Can you list files in /tmp?                       │
│  Chat 2  💾 🗑      │                                                    │
│  Chat 3  💾 🗑      │  ┌─ Tool block (collapsed) ──────────────────┐    │
│                    │  │ ▶ Tool: run_bash [command='ls /tmp']       │    │
│  ─────────────     │  └────────────────────────────────────────────┘   │
│  Status ● Idle     │                                                    │
│  Model: gpt-4o     │  🤖 Assistant                                      │
│  Tool Confirm: None│  Here are the files in /tmp: ...                   │
│  Tokens: — in/out  │                                                    │
│                    │  ──────────────────────────────────────────────    │
│                    │  [Type a message...                  ] [⏹ Stop]   │
└────────────────────┴────────────────────────────────────────────────────┘
```

---

## LLM Client

`LlmClient::run()` is a synchronous blocking call designed to be called from a `QThread`. It drives the full conversation loop:

1. POST messages to `/v1/chat/completions` via `syncPost()` (uses a local `QEventLoop` — safe on any thread)
2. Parse `choices[0]` — if `tool_calls` present, enter tool loop; otherwise emit `final_response`
3. For each tool call: check `tool_confirmation`, optionally block for user approval, call `Tools::execute()`
4. Append assistant + tool result messages and repeat from step 1

Events are reported via `std::function<void(const QJsonObject&)>` callbacks (called on the worker thread; `MainWindow` uses `Qt::QueuedConnection` to receive them on the main thread via `ChatWorker`).

### Event Types

| Event type | Payload fields |
|---|---|
| `assistant_tool_calls` | `tool_calls` array |
| `tool_request` | `tool_name`, `tool_args` |
| `tool_result` | `tool_name`, `tool_result` |
| `final_response` | `content`, `usage` (prompt/completion tokens) |

---

## Tool Confirmation Flow

```
LLM responds with tool_calls
       │
       ├─ tool_confirmation = "all" ──► auto-approve → execute → loop
       │
       ├─ tool_confirmation = "safe" & tool is read-only ──► auto-approve → loop
       │
       └─ Otherwise
              │
              ▼
        Modal dialog (tool name + full JSON args)
              │
              ├── Execute              → execute → feed result → loop
              ├── Yes to All This Turn → execute + yolo for rest of turn → loop
              └── Decline              → "Tool execution was declined by user." → loop
```

`ChatWorker` blocks on `QWaitCondition::wait()` while the dialog is shown. The main thread calls `sendConfirmation(confirmed, yoloTurn)` which sets `m_confirmed`/`m_yoloTurn` and calls `m_confirmWait.wakeAll()`.

---

## ChatView Rendering

`ChatView` (`QTextBrowser`) renders messages as HTML with inline CSS.

| Message type | Appearance |
|---|---|
| User | Bold dark-blue `🧑 You` label, plain HTML-escaped body |
| Assistant | Bold dark-green `🤖 Assistant` label, markdown-converted body |
| Tool block (collapsed) | `▶ Tool: name [args…]` — clickable toggle |
| Tool block (expanded) | `▼ Tool: name` + **Arguments:** `<pre>` + **Result:** `<pre>` |

### Markdown Pipeline (`markdownToHtml`)

1. HTML-escape entire input (`toHtmlEscaped()`)
2. Fenced code blocks → `<pre><code>`
3. Markdown tables → `<table>`
4. Inline code → `<code>`
5. Bold / italic → `<b>` / `<i>`
6. Qt table fix → `<table cellspacing="0">` (Qt doesn't support `border-collapse: collapse`)
7. Paragraphize — `\n\n` → `<p>`, single `\n` → `<br>`, block elements left intact

---

## Chat Sidebar

Each chat row in `ChatHistory` is a custom `QWidget` set via `setItemWidget()`:

```
[ Chat title (expanding)          ] [💾] [🗑]
```

- **💾** — Saves the chat as a Markdown file (`QFileDialog::getSaveFileName`, pre-filled with the chat title)
- **🗑** — Emits `deleteRequested(id)` → `MainWindow::deleteChat()`

---

## Data Storage

Shared with Python and Rust Pengy at `~/.config/pengy/`.

### `settings.json`

```json
{
  "base_url": "https://api.openai.com/v1",
  "api_key": "",
  "model": "gpt-4o",
  "system_message": "You are a helpful assistant. The current date is {date} and the user is {username} on host {hostname} which is {osinfo}.",
  "tool_confirmation": "none",
  "context_keep_turns": 0,
  "ui_scale": 100,
  "user_agent": "PengyAgent/1.0",
  "tool_timeout": 60
}
```

`{date}`, `{username}`, `{hostname}`, `{osinfo}` are resolved at send time via `configRenderSystemMessage()`.

### `chats.json`

Array of chat objects. Message schema matches Python Pengy exactly: `user`, `assistant` (with optional `tool_calls`), and `tool` role messages.

---

## Tools

All 11 tools implemented in `tools.cpp` using Qt APIs:

| Tool | Read-only | Implementation |
|------|:---:|---|
| `read_file` | ✅ | `QFile` |
| `read_multiple_files` | ✅ | `QFile` × N |
| `write_file` | ❌ | `QFile` + `QDir::mkpath` |
| `replace_in_file` | ❌ | Read→exact-match→replace→write |
| `run_bash` | ❌ | `QProcess` (sudo via `-S`) |
| `run_python` | ❌ | Write to temp + `QProcess python3` |
| `web_search` | ✅ | DuckDuckGo HTML scrape via `QNetworkAccessManager` + `QRegularExpression` |
| `download_file` | ❌ | `QNetworkAccessManager` → `~/Downloads/` |
| `fetch_url` | ✅ | `QNetworkAccessManager` + HTML tag-strip |
| `directory_tree` | ✅ | `QDirIterator` with Unicode box-drawing |
| `search_content` | ✅ | `QDirIterator` + `QRegularExpression` with context lines |

`Tools::isReadOnly(name)` is used by the tool confirmation logic to auto-approve in `"safe"` mode.

---

## Build & Packaging

### Linux

```bash
sudo apt install build-essential cmake qt6-base-dev libgl-dev
./build_linux.sh
# → build/pengy  (~8 MB, Qt6 linked dynamically)
```

### macOS

```bash
brew install qt@6 cmake
./build_macos.sh [arm64|x86_64]
# → Pengy.app  (macdeployqt bundles Qt frameworks)
```

### Windows

```
REM From a VS 2022 Developer Command Prompt:
build_windows.bat
REM → Pengy-Windows\  (windeployqt bundles Qt DLLs)
```

---

## Dependencies

| Dependency | Purpose |
|---|---|
| Qt6::Core | Foundation: `QFile`, `QDir`, `QJson*`, `QProcess`, `QRegularExpression` |
| Qt6::Widgets | GUI: `QMainWindow`, `QListWidget`, `QTextBrowser`, `QSplitter`, dialogs |
| Qt6::Network | HTTP: `QNetworkAccessManager`, `QNetworkReply` |
| C++17 compiler | `std::function`, `std::pair`, `std::atomic`, structured bindings |
| CMake ≥ 3.16 | Build system |

No Rust, no Python, no third-party C++ libraries.

---

## Design Decisions

**Pure C++ instead of Rust+FFI:** Eliminates the Rust toolchain dependency and the complexity of the C FFI boundary. All logic (config, chat manager, tools, LLM client) lives in the same CMake project, debuggable with standard C++ tools.

**Blocking HTTP via local QEventLoop:** `QNetworkAccessManager` is inherently async, but creating a local `QEventLoop` and `QNetworkReply::finished` connection makes it synchronous from the caller's perspective. This works correctly on `QThread` worker threads without a persistent event loop.

**QWaitCondition for tool confirmation:** The worker thread calls `m_confirmWait.wait(&m_mutex)` and suspends with zero CPU usage. The main thread shows the dialog, collects user input, sets result fields, and calls `m_confirmWait.wakeAll()`. No polling, no spin loop.

**Schema compatibility:** Chat messages use the same JSON format as Python Pengy, so chats created in any version can be opened in any other.

---

## Feature Gaps (vs Python Pengy)

| Feature | Status | Notes |
|---------|:---:|---|
| File attachments | ❌ | Not yet ported |
| Image paste from clipboard | ❌ | Not yet ported |
| CLI (rich-based terminal REPL) | ❌ | Python-only |
| Web UI (Flask + SSE) | ❌ | Python-only |
| Skills system | ❌ | Python-only |
| Image download rendering | ❌ | `QTextBrowser::loadResource` not yet connected |
| Context elision | ❌ | `elideOldToolResults` exists but not wired to config |

---

## License

MIT
