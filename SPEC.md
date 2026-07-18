# PengyCPP — Application Specification

## Overview

PengyCPP is a pure C++17/Qt6 port of [Pengy](https://github.com/patw/pengy) — a local-first AI agent application that connects to any OpenAI-compatible LLM API and gives the model a set of tools to operate on the user's machine. Three frontends: Qt6 desktop GUI, CLI, and Web UI.

> PengyCPP shares `~/.config/pengy/` with the Python Pengy and PengyR. Settings and chat history are fully interoperable between all three applications.

---

## Technology Stack

- **Language:** C++17 (single CMake project, no external runtimes)
- **GUI Framework:** Qt6 Widgets
- **HTTP:** `QNetworkAccessManager` + local `QEventLoop` (synchronous-style calls on worker threads)
- **JSON:** `QJsonDocument` / `QJsonObject` / `QJsonArray`
- **Threading:** `QThread::create()` lambda + `QMutex` / `QWaitCondition` for tool confirmation
- **Web Server:** `QTcpServer` (raw HTTP parsing + SSE, no external web framework)
- **Markdown Rendering:** `QTextBrowser` with custom regex pipeline (GUI); simple regex converter (Web)
- **CLI:** `QTextStream` on stdio; ANSI color codes
- **Storage:** JSON files in `~/.config/pengy/` (shared with Python Pengy and PengyR)

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│  Frontends (three binaries from one CMake project)                   │
│                                                                      │
│  ┌─ Qt6 GUI (pengy) ─────────────┐  ┌─ CLI (pengy_cli) ──────────┐ │
│  │ ChatHistory / ChatView /       │  │ Interactive REPL           │ │
│  │ ChatInput / SettingsDialog     │  │ Single-shot mode           │ │
│  │ ChatWorker (QThread)           │  │ Slash commands             │ │
│  └────────────┬───────────────────┘  └────────────┬──────────────┘ │
│               │                                   │                 │
│               │  LlmClient::run()                  │  LlmClient::run()│
│               │  (on QThread)                      │  (main thread)  │
│               │                                   │                 │
│  ┌─ Web UI (pengy_web) ──────────┐                │                 │
│  │ Bootstrap 5 + SSE streaming    │                │                 │
│  │ WebChatWorker (QThread)        │                │                 │
│  │ WebServer (QTcpServer)         │                │                 │
│  └────────────┬───────────────────┘                │                 │
│               │  LlmClient::run()                  │                 │
│               │  (on QThread)                      │                 │
├───────────────┴────────────────────────────────────┴─────────────────┤
│  C++17 Core (shared by all three binaries)                           │
│  ┌──────────┐ ┌──────────────┐ ┌──────────────┐                    │
│  │  Config  │ │  ChatManager │ │    Tools     │                    │
│  │ config.* │ │chatmanager.* │ │   tools.*    │                    │
│  └──────────┘ └──────────────┘ └──────────────┘                    │
│  ┌────────────────────────────────────────────────┐                 │
│  │ LlmClient (blocking chat loop, QNetworkAccess)  │                 │
│  └────────────────────────────────────────────────┘                 │
└──────────────────────────────────────────────────────────────────────┘
```

### Source Layout

```
PengyCPP/
├── CMakeLists.txt          # Single CMake project — builds pengy, pengy_cli, pengy_web, pengy_tests
├── main.cpp                # Desktop GUI entry point — QApplication setup
├── config.cpp/h            # Settings load/save + system message rendering
├── chatmanager.cpp/h       # Chat session CRUD + message cleaning + context elision
├── tools.cpp/h             # 11 OpenAI function-calling tools (Qt APIs)
├── llmclient.cpp/h         # Blocking LLM chat loop (QNetworkAccessManager + QEventLoop)
├── chatworker.cpp/h        # QThread worker — runs LlmClient + confirmation QWaitCondition
├── mainwindow.cpp/h        # Three-pane main window, tool confirmation dialog
├── chathistory.cpp/h       # Left sidebar — chat list with 💾/🗑 buttons
├── chatview.cpp/h          # Right-top — QTextBrowser markdown, tables, collapsible tool blocks
├── chatinput.cpp/h         # Right-bottom — message input (QPlainTextEdit)
├── settingsdialog.cpp/h    # Settings modal + Fetch Models button
├── taskmanager.cpp/h       # Prompt-template Tasks CRUD (~/.config/pengy/tasks.json)
├── tasksdialog.cpp/h       # Tasks manager/player dialog
├── themehelper.h           # Light/dark/accent theme + UI scale helpers
├── cli/
│   └── main.cpp            # Interactive REPL + single-shot mode + slash commands
├── web/
│   ├── main.cpp            # Web server entry point (default port 5000)
│   ├── webserver.cpp/h     # QTcpServer HTTP + SSE server + routing
│   ├── webchatworker.cpp/h # QThread worker for web (mirrors ChatWorker pattern)
│   ├── web_resources.qrc   # Embeds HTML templates into binary
│   └── templates/
│       ├── chat.html       # Bootstrap 5 chat UI with SSE JavaScript client
│       └── settings.html   # Settings form
├── tests.cpp               # Qt Test suite (60+ tests)
├── appimage/
│   ├── build.sh            # Bundles Pengy-x86_64.AppImage
│   └── pengy.desktop       # Linux desktop entry
├── build_linux.sh          # Linux native build
├── build_macos.sh          # macOS build + Pengy.app + DMG
├── build_windows.bat       # Windows build (MSVC Qt6)
└── build_deb.sh            # Debian/Ubuntu .deb package
```

---

## Core Modules

All three binaries share these modules — no code duplication, no FFI:

| Module | Files | Responsibility |
|--------|-------|---------------|
| `Config` | `config.cpp/h` | Load/save `~/.config/pengy/settings.json` with default merging; `configRenderSystemMessage()` fills `{date}`, `{username}`, `{hostname}`, `{osinfo}` at send time |
| `ChatManager` | `chatmanager.cpp/h` | CRUD for `~/.config/pengy/chats.json`; `cleanDanglingToolCalls()` removes orphaned tool_calls; `elideOldToolResults()` replaces old tool content with `[elided]` |
| `Tools` | `tools.cpp/h` | 11 OpenAI function-calling tool schemas and execution via Qt APIs; `isReadOnly()` classification; sudo password provider callback |
| `LlmClient` | `llmclient.cpp/h` | Blocking chat loop via `QNetworkAccessManager` + local `QEventLoop`; emits events via `std::function` callbacks |

---

## Desktop UI Layout

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

### Left Pane (Sidebar)
- **+ New Chat button** — Creates a new chat session
- **⚙ Settings button** — Opens the settings dialog
- **Chat history list** — Scrollable, sorted newest first; click to load
- **Per-row buttons** — 💾 (export chat to Markdown file) and 🗑 (delete chat)
- **Quick settings panel** — Shows status dot (● idle / ● running), model name, tool confirmation mode, and last turn token counts

### Right-Top Pane (Chat View)
- Markdown-rendered chat messages via `QTextBrowser`
- **User messages:** bold dark-blue `🧑 You` label, plain body text
- **Assistant messages:** bold dark-green `🤖 Assistant` label, markdown-converted body
- **Tool blocks:** collapsed by default (`▶ Tool: name [args…]`); click to expand and show args + result
- Syntax-highlighted code blocks via custom CSS
- Image rendering: pasted images and downloaded images display inline
- Auto-scrolls to bottom on new content

### Right-Bottom Pane (Chat Input)
- **📎 Attach button** — Opens file picker; accepts text files and images
- **Image paste from clipboard** — Pasted images saved to temp file, sent as base64 data URIs
- **File chips** — Selected files shown as removable badges above the input
- **Text input** — Multi-line QPlainTextEdit; Enter to send, Shift+Enter for newline
- **⏹ Stop button** — Cancels the running LLM conversation; visible only during generation

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

### Tool Block Toggling

Tool calls are stored as unified `tool_block` messages (request + result combined). A `QSet<QString>` tracks which blocks are expanded. Clicking the `▶/▼` toggle link triggers `anchorClicked` which flips the set and rebuilds the full HTML.

### Image Rendering

`QTextBrowser::loadResource` is overridden to fetch and cache HTTP images referenced in markdown. Pasted clipboard images are saved to temp files and referenced as local `file://` URLs.

---

## CLI Interface

The CLI binary (`pengy_cli`) provides an interactive REPL and single-shot mode. It uses the core modules directly — no threading.

### Entry Points

```bash
# Interactive REPL
pengy_cli

# Single-shot
pengy_cli "What is the capital of France?"

# Single-shot without saving
pengy_cli --no-save "quick question"
```

Flags (shared with the Python and Rust CLIs): `--no-save`, `--model NAME`, `--system MSG`, `--output pretty|raw|json|silent`, `--config-dir PATH`, `-v/--version`. `--model` and `--system` are in-memory overrides — they never modify `settings.json`.

### Interactive Mode

On startup:
1. Loads the most recent chat from `chats.json` (or creates a new one if none exist)
2. Shows a welcome banner with model name and tool confirmation status
3. Enters the REPL loop: prompt → send → LlmClient::run() → loop

The `PengyCliApp` class drives `LlmClient::run()` on the main thread. Tool confirmation blocks on `QTextStream` input with a 3-choice menu: Execute / Yes to all this turn / Decline.

### Single-Shot Mode

1. Creates a throw-away chat (persisted unless `--no-save` is passed)
2. Sends the prompt, drives the conversation to completion, and exits
3. Useful for scripting: `pengy_cli "summarize this file" && pengy_cli "translate to French"`

### Slash Commands

| Command | Description |
|---------|-------------|
| `/help` | Show the command reference table |
| `/new` | Start a new chat session |
| `/show [n]` | Show the full conversation (optional: last n messages) |
| `/tail [n]` | Show the last n messages (default 5) |
| `/rename <title>` | Rename the current chat |
| `/clear` | Clear the terminal screen |
| `/export [path]` | Export the current chat as Markdown |
| `/yolo [all\|safe\|none]` | Set tool confirmation: all (YOLO), safe (read-only), none — cycles if no arg |
| `/config` | Show current configuration (base URL, model, timeout, etc.) |
| `/model <name>` | Switch models (e.g. `/model gpt-4o`) |
| `/models` | Fetch available models from the endpoint's `GET /v1/models` |
| `/baseurl <url>` | Change the API base URL |
| `/apikey <key>` | Set the API key |
| `/timeout <sec>` | Set tool execution timeout |
| `/agent <string>` | Set the user agent string |
| `/context-keep <n>` | Set context elision keep-turns (0 = keep all) |
| `/system [message]` | Show or set the system message template |
| `/list` | List recent chats with index, title, message count, and creation date |
| `/load <index>` | Load a chat by its `/list` index |
| `/delete <index>` | Delete a chat by its `/list` index |
| `/compact` | Elide old tool results to free context window space |
| `/attach <path>` | Attach a text file (or use `@path` inline in your prompt) |
| `/quit`, `/exit`, `/q` | Exit the CLI |

### File Attachments

The `@path/to/file` syntax anywhere in a message reads the file's contents and injects it as a fenced code block before the user's prompt.

---

## Web Interface

The Web binary (`pengy_web [port]`) runs a `QTcpServer` HTTP server (default port 5000) with a Bootstrap 5 UI. Intended for single-user personal use; SSL and authentication are expected to be handled by a reverse proxy (nginx).

### Entry Points

```bash
pengy_web                            # localhost:5000
pengy_web 8080                       # custom port (positional)
pengy_web 8080 --host 0.0.0.0        # expose beyond localhost (no auth — trusted networks only)
pengy_web --config-dir PATH          # custom config directory
pengy_web -h                         # help
```

The server prints its URL on startup; it does not auto-open a browser.

### Layout

```
┌──navbar: 🐧 PengyCPP  [model] [Confirm badge]  [⚙]──────────┐
│                                                               │
│  ┌─sidebar (260px)─┐  ┌─chat area──────────────────────┐    │
│  │  [+ New Chat]   │  │  message history (scrollable)  │    │
│  │                 │  │                                │    │
│  │  Chat 1   [×]  │  │  User bubble (right-aligned)   │    │
│  │  Chat 2   [×]  │  │  🔧 tool card (collapsed)       │    │
│  │  Chat 3   [×]  │  │  Assistant bubble (markdown)   │    │
│  └─────────────────┘  │                                │    │
│   (offcanvas on mob.) │  ┌──input + [Send]────────────┐│    │
│                        │  └────────────────────────────┘│    │
│                        └────────────────────────────────┘    │
└───────────────────────────────────────────────────────────────┘
```

- **Sidebar** — Fixed column on md+ screens; offcanvas drawer on mobile. Lists all chats with delete button; "New Chat" button at top.
- **Chat area** — Server-rendered message history on page load; SSE appends new content live during generation.
- **Input** — Auto-expanding textarea; Enter to send, Shift+Enter for newline; 📎 attach button (files sent base64, injected as fenced blocks); `/` commands handled via `POST /chat/:id/command`.
- **Navbar** — Shows current model and tool confirmation mode badge; export (⬇) button downloads the chat as Markdown; double-click the brand to rename; gear icon links to settings.

### Routes

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Redirect to most recent chat (or create new) |
| POST | `/chat/new` | Create a new chat session |
| GET | `/chat/:id` | Render chat page (server-side history) |
| POST | `/chat/:id/send` | Append user message, start WebChatWorker |
| GET | `/chat/:id/stream` | SSE endpoint — streams events until final response |
| POST | `/chat/:id/confirm` | Unblock tool confirmation (confirmed/declined/yolo) |
| POST | `/chat/:id/sudo` | Provide sudo password to blocked worker |
| POST | `/chat/:id/delete` | Delete chat and redirect to index |
| GET | `/chat/:id/export` | Download the chat as a Markdown file |
| POST | `/chat/:id/rename` | Rename a chat |
| POST | `/chat/:id/command` | Web slash commands (`/new /yolo /model /rename /export /help`) typed in the chat input |
| GET | `/models` | Fetch available models from the endpoint (settings page Fetch button) |
| GET/POST | `/settings` | View/update all config fields |
| POST | `/chat/:id/stop` | Cancel running generation for a chat |

### SSE Event Types

| Type | Payload | Browser action |
|------|---------|---------------|
| `tool_request` | `name`, `args`, `auto_approved` | Append tool card; if not auto-approved, show confirmation modal |
| `tool_result` | `content`, `declined` | Update tool card body and badge |
| `final_response` | `html`, `usage` | Append assistant bubble |
| `sudo_request` | — | Show sudo password modal |
| `error` | `message` | Append error alert, re-enable input |
| `keepalive` | — | SSE comment (`: keepalive`); browser ignores |

### Tool Confirmation Flow (Web)

```
SSE sends tool_request (auto_approved=false)
       │
       ▼
Browser shows Bootstrap modal (tool name + args JSON)
       │
       ├── Execute              → POST /confirm {confirmed: true}
       ├── Yes to all this turn → POST /confirm {confirmed: true, yolo_turn: true}
       └── Decline              → POST /confirm {confirmed: false}
              │
              ▼
       WebChatWorker QWaitCondition wakes → LlmClient resumes
```

### WebChatWorker

`WebChatWorker` mirrors `ChatWorker`'s pattern. It runs `LlmClient::run()` on a `QThread` and emits events via Qt signals (`eventReady`, `sudoRequired`). It enriches `tool_request` events with `auto_approved` by replicating LlmClient's skip-confirm logic. Events are queued in `WebServer::m_eventQueue` if the SSE client hasn't connected yet, then flushed when the SSE connection opens.

HTML templates (`chat.html`, `settings.html`) are embedded in the binary via `web/web_resources.qrc` — no external files needed at runtime.

---

## LLM Client

`LlmClient::run()` is a synchronous blocking call designed to be called from **any thread** (not just the main thread). It drives the full conversation loop:

1. POST messages to `/v1/chat/completions` via `syncPost()` — uses a stack-local `QNetworkAccessManager` + `QEventLoop` (safe on any thread)
2. Parse `choices[0]` — if `tool_calls` present, enter tool loop; otherwise emit `final_response`
3. For each tool call: check `tool_confirmation`, optionally block for user approval, call `Tools::execute()`
4. Append assistant + tool result messages and repeat from step 1

Events are reported via `std::function<void(const QJsonObject&)>` callbacks:

| Event type | Payload fields |
|---|---|
| `assistant_tool_calls` | `tool_calls` array |
| `tool_request` | `tool_name`, `tool_args`, `tool_call_id` |
| `tool_result` | `tool_name`, `tool_args`, `tool_call_id`, `content`, `declined` |
| `final_response` | `content`, `usage` (prompt/completion tokens) |

**Caller responsibilities:** Callers must accumulate message history from events:
- `assistant_tool_calls` → append the full assistant message object to history
- `tool_result` → append `{"role":"tool","tool_call_id":...,"content":...}`
- `final_response` → append `{"role":"assistant","content":...}`

---

## Message Flow

```
User types message → Enter
       │
       ▼
User message appended to chat view and message history
       │
       ▼
System message rendered (templates filled) and prepended
       │
       ▼
cleanDanglingToolCalls() removes orphaned tool_calls from history
       │
       ▼
elideOldToolResults() replaces old tool content with [elided] (if context_keep_turns > 0)
       │
       ▼
LLM API call (non-streaming, full response at once)
       │
       ├── No tool calls → render final response → save chat
       │
       └── Tool call(s) → confirm/execute loop → final response → save chat
```

**Note:** The system message is **not** stored in `chat["messages"]` — it is prepended at request time so templates are always fresh.

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

`ChatWorker` blocks on `QWaitCondition::wait()` (zero CPU usage) while the dialog is shown. The main thread calls `sendConfirmation(confirmed, yoloTurn)` which sets the result fields and calls `m_confirmWait.wakeAll()`.

### Sudo Password Flow

When `run_bash` detects `sudo` in the command:

```
Tool execution reaches run_bash
       │
       ├─ No "sudo" in command ──► execute normally
       │
       └─ "sudo" detected
              │
              ├─ Password cached? ──► use cached password
              │
              └─ No cache
                     │
                     ▼
              Tools calls sudo_password_provider callback
                     │
                     ▼
              ChatWorker blocks on sudo QWaitCondition
                     │
                     ▼
              MainWindow::pollToolConfirmation() (100ms QTimer)
                     │
                     ▼
              QInputDialog::getText (password mode)
                     │
                     ├── OK with password → ChatWorker::sendSudoPassword()
                     └── Cancel → ChatWorker::cancelSudo()
```

The password is:
1. Cached for the duration of the LLM run
2. Injected via stdin after rewriting `sudo` → `sudo -S`
3. Cleared when the LLM run completes

### Process-Group Kill

On timeout or cancel, `run_bash` issues `kill -9 -PID` (killing the entire process group) before calling `QProcess::kill()`. This prevents orphaned child processes from surviving after a timeout.

---

## Settings Dialog (Desktop)

| Field | Widget | Notes |
|-------|--------|-------|
| Base URL | QLineEdit | OpenAI-compatible endpoint |
| API Key | QLineEdit (masked) | Stored in settings.json (plaintext) |
| Model | QComboBox (editable) | Pre-populated with current model; "Fetch" button calls `GET /v1/models` |
| System Message | QTextEdit | Supports `{date}`, `{username}`, etc. templates |
| Tool Confirmation | QComboBox | "YOLO (All)", "Safe Only", "None" |
| Context Keep Turns | QSpinBox | Number of recent turns to keep tool results for (0 = keep all) |
| UI Scale | QComboBox | 75%, 100%, 125%, 200% — takes effect on relaunch |
| Tool Timeout | QSpinBox | Seconds (-1 = no timeout) |

---

## Data Storage

Shared with Python Pengy and PengyR at `~/.config/pengy/`.

### Settings File: `~/.config/pengy/settings.json`

```json
{
  "base_url": "https://api.openai.com/v1",
  "api_key": "",
  "model": "gpt-4o",
  "system_message": "You are a helpful assistant named Pengy. The current date is {date} and the user is {username} on host {hostname} which is {osinfo}.",
  "tool_confirmation": "none",
  "reasoning_effort": "",
  "preserve_reasoning": false,
  "context_keep_turns": 0,
  "ui_scale": 100,
  "theme_mode": "system",
  "theme_accent": "default",
  "user_agent": "PengyAgent/1.0",
  "tool_timeout": 60
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `base_url` | string | `https://api.openai.com/v1` | OpenAI-compatible API endpoint |
| `api_key` | string | (empty) | API key |
| `model` | string | `gpt-4o` | Model name |
| `system_message` | string | (see above) | Template; `{date}`, `{username}`, `{hostname}`, `{osinfo}` filled at send time |
| `tool_confirmation` | string | `"none"` | `"all"` (YOLO), `"safe"` (read-only auto), `"none"` (prompt all) |
| `reasoning_effort` | string | `""` | Passed as `reasoning_effort` on API calls when set (`none`…`max`; `""` = provider default) |
| `preserve_reasoning` | bool | `false` | Keep reasoning fields on assistant messages sent back to the API |
| `context_keep_turns` | int | `0` | Recent turns whose tool results are kept; older ones elided. 0 = keep all |
| `ui_scale` | int | `100` | Sets `QT_SCALE_FACTOR` on next launch (75/100/125/200); CLI ignores |
| `theme_mode` | string | `"system"` | Desktop theme: `"system"`, `"light"`, or `"dark"` |
| `theme_accent` | string | `"default"` | Desktop accent color (`default`/`blue`/`teal`/`green`/`orange`/`red`/`pink`/`purple`) |
| `user_agent` | string | `PengyAgent/1.0` | User-Agent header for HTTP requests |
| `tool_timeout` | int | `60` | Timeout in seconds for tool execution (-1 = no timeout) |

### System Message Templating

`configRenderSystemMessage()` is called at send time (not at save time), so `{date}` always reflects today. Variables:

| Placeholder | Source |
|------------|--------|
| `{date}` | `QDate::currentDate().toString("MMMM dd, yyyy")` |
| `{username}` | `qgetenv("USER")` |
| `{hostname}` | `QSysInfo::machineHostName()` |
| `{osinfo}` | `QSysInfo::prettyProductName()` |

### Chats File: `~/.config/pengy/chats.json`

Array of chat session objects with `user`, `assistant` (including `tool_calls`), and `tool` messages. Format is identical to Python Pengy and PengyR.

---

## Tools

All 11 tools implemented in `tools.cpp` using Qt APIs:

| Tool | Read-only | Implementation |
|------|:---:|---|
| `read_file` | ✅ | `QFile` |
| `read_multiple_files` | ✅ | `QFile` × N (20 file max, 50K char per file cap) |
| `write_file` | ❌ | `QFile` + `QDir::mkpath` |
| `replace_in_file` | ❌ | Read→exact-match→replace→write (single-match enforcement) |
| `run_bash` | ❌ | `QProcess` (sudo via `-S` + stdin, process-group kill on timeout) |
| `run_python` | ❌ | Write to temp + `QProcess python3` |
| `web_search` | ✅ | DuckDuckGo HTML scrape via `QNetworkAccessManager` + `QRegularExpression` |
| `download_file` | ❌ | `QNetworkAccessManager` → `~/Downloads/` |
| `fetch_url` | ✅ | `QNetworkAccessManager` + HTML tag-strip (50K char limit) |
| `directory_tree` | ✅ | `QDirIterator` with Unicode box-drawing (500 entry cap) |
| `search_content` | ✅ | `QDirIterator` + `QRegularExpression` with context lines and region grouping |

`Tools::isReadOnly(name)` is used by the tool confirmation logic to auto-approve in `"safe"` mode. Read-only: `read_file`, `read_multiple_files`, `directory_tree`, `search_content`, `web_search`, `fetch_url`. Mutating: everything else.

---

## App Identity

- **Application name:** "Pengy" (set via `QApplication::setApplicationName("Pengy")`)
- **Icon:** `pengy.png` (256×256) — loaded at startup via `QApplication::setWindowIcon`
- The desktop app shows in taskbar, alt-tab, and window decorations on X11/XWayland. On native Wayland, the provided `pengy.desktop` file may be needed for taskbar icon.
- The CLI uses no icon but displays the penguin emoji (🐧) in its welcome banner.
- The Web UI title bar shows "🐧 PengyCPP".

---

## Build & Packaging

### Linux

```bash
sudo apt install build-essential cmake qt6-base-dev libgl-dev
./build_linux.sh
# → build/pengy  (~8 MB, Qt6 linked dynamically)
# → build/pengy_cli
# → build/pengy_web
```

### Linux AppImage (GUI only)

```bash
# Download linuxdeploy tools (one time):
wget -P appimage/tools https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
wget -P appimage/tools https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
chmod +x appimage/tools/*.AppImage

cd appimage && ./build.sh
# → Pengy-x86_64.AppImage
```

### Linux .deb

```bash
./build_deb.sh
# → pengy_<version>_amd64.deb
sudo dpkg -i pengy_<version>_amd64.deb
```

### macOS

```bash
brew install qt@6 cmake
./build_macos.sh [arm64|x86_64]
# → Pengy.app  (macdeployqt bundles Qt frameworks)
# → Pengy-macOS-<arch>.dmg
```

### Windows

```
REM From a VS 2022 Developer Command Prompt:
build_windows.bat
REM → Pengy-Windows\pengy.exe  (windeployqt bundles Qt DLLs)
REM → Pengy-Windows.zip
```

---

## Design Decisions

**Pure C++17 + Qt6 — no Rust, no Python, no FFI:** Eliminates all external toolchain dependencies. All logic (config, chat manager, tools, LLM client) lives in one CMake project, debuggable with standard C++ tools. Three binaries share source files directly (no library boundary).

**Blocking HTTP via local QEventLoop:** `QNetworkAccessManager` is inherently async, but creating a stack-local `QEventLoop` and connecting `QNetworkReply::finished` makes it synchronous from the caller's perspective. This works correctly on any thread without a persistent event loop.

**QWaitCondition for tool confirmation:** The worker thread calls `m_confirmWait.wait(&m_mutex)` and suspends with zero CPU usage. The main thread shows the dialog, collects user input, sets result fields, and calls `m_confirmWait.wakeAll()`. No polling, no spin loop. This is an improvement over PengyR's 5ms spin approach.

**Schema compatibility:** Chat messages use the same JSON format as Python Pengy, so chats created in any version load in any other.

**Non-streaming API calls:** The LLM client uses non-streaming completions (no `stream: true`). Full responses render at once. This simplifies the architecture and is acceptable because tool call round-trips dominate latency for agentic workflows.

**Sudo via `-S` with QWaitCondition:** Same approach as Python Pengy — detect `sudo` in bash commands, prompt for password via `QInputDialog`, pass to `sudo -S`. Password cached per LLM run. The main thread polls with a 100ms `QTimer` and shows the password dialog.

**Process-group kill:** On timeout or cancel, `run_bash` issues `kill -9 -PID` before `QProcess::kill()`, preventing orphaned child processes.

**Context elision:** `elideOldToolResults()` is called after `cleanDanglingToolCalls()` and before every API request, controlled by `context_keep_turns` (0 = keep all). This keeps context window usage under control for long conversations.

**QTcpServer for Web UI:** Instead of pulling in an HTTP library, the web UI uses Qt's built-in `QTcpServer` with manual HTTP parsing and SSE handling. This keeps dependencies at zero beyond Qt6. HTML templates are embedded in the binary via Qt Resources.

**Chat export to Markdown:** The 💾 button in the sidebar exports any chat to a `.md` file via `QFileDialog::getSaveFileName`, pre-filled with the chat title. All three Pengy editions share this feature.

**Single CMake project:** All three binaries (`pengy`, `pengy_cli`, `pengy_web`) and tests (`pengy_tests`) are built from one `CMakeLists.txt`. Source files are shared directly — no library, no FFI, no subprojects. CLI and Web link only `Qt6::Core` + `Qt6::Network`; GUI also links `Qt6::Widgets`.

---

## Feature Parity

| Feature | Pengy (Python) | PengyR (Rust) | PengyCPP |
|---------|:---:|:---:|:---:|
| OpenAI-compatible LLM API | ✅ | ✅ | ✅ |
| 11 tools (bash, python, files, web, etc.) | ✅ | ✅ | ✅ |
| Three-pane Qt6 desktop GUI | ✅ | ✅ | ✅ |
| Markdown rendering + syntax highlighting | ✅ | ✅ | ✅ |
| Collapsible tool call blocks | ✅ | ✅ | ✅ |
| Chat history sidebar (CRUD) | ✅ | ✅ | ✅ |
| Chat export to Markdown (💾 button) | ✅ | ✅ | ✅ |
| Settings dialog + Fetch Models | ✅ | ✅ | ✅ |
| Tool confirmation (YOLO / Safe / None) | ✅ | ✅ | ✅ |
| Sudo password support | ✅ | ✅ | ✅ |
| Process-group kill on timeout | — | — | ✅ |
| File attachments (GUI) | ✅ | ✅ | ✅ |
| Image paste from clipboard | ✅ | ✅ | ✅ |
| Image download rendering | ✅ | ✅ | ✅ |
| CLI (interactive REPL + single-shot) | ✅ | ✅ | ✅ |
| Web UI (SSE streaming) | ✅ | ✅ | ✅ |
| Web slash commands (`/command` route) | ✅ | ✅ | ✅ |
| Web file attachments | ✅ | ✅ | ✅ |
| Web chat export + rename | ✅ | ✅ | ✅ |
| Web Fetch Models (`/models` route) | ✅ | ✅ | ✅ |
| Tasks (prompt templates, GUI dialog) | ✅ | ✅ | ✅ |
| Theme system (mode + accent) | ✅ | ✅ | ✅ |
| Reasoning effort / preservation | ✅ | ✅ | ✅ |
| Context elision | ✅ | ✅ | ✅ |
| Skills system | ✅ | ✅ | ✅ |

---

## Dependencies

| Dependency | Purpose |
|---|---|
| Qt6::Core | Foundation: `QFile`, `QDir`, `QJson*`, `QProcess`, `QRegularExpression` |
| Qt6::Widgets | GUI: `QMainWindow`, `QListWidget`, `QTextBrowser`, `QSplitter`, dialogs |
| Qt6::Network | HTTP: `QNetworkAccessManager`, `QNetworkReply` |
| C++17 compiler | `std::function`, `std::pair`, `std::atomic`, structured bindings |
| CMake ≥ 3.16 | Build system |

**No Rust, no Python, no third-party C++ libraries.** Everything is Qt6 + C++17 STL.

---

## Design Differences from PengyR

| Aspect | PengyR | PengyCPP |
|--------|--------|----------|
| Language | Rust core + C++ GUI via FFI | Pure C++17, no FFI |
| HTTP client | `reqwest` (Rust) | `QNetworkAccessManager` (Qt) |
| Tool confirmation sync | Shared struct + 5ms spin | `QWaitCondition` + `QMutex` (zero CPU) |
| Sudo prompt | `rpassword` (Rust) / shared struct spin | `QWaitCondition` + 100ms QTimer poll |
| Process cleanup | `QProcess::kill()` only | `kill -9 -PID` + `QProcess::kill()` |
| Web server | Axum (Rust) | `QTcpServer` (Qt) |
| Markdown in web | regex-based (Rust) | regex-based (C++) |
| Chat export | 💾 button in sidebar | 💾 button in sidebar |
| Binary size (GUI) | ~13 MB (Rust statically linked) | ~8 MB (pure C++, Qt dynamic) |
| Rust toolchain required | Yes | No |

---

## License

MIT
