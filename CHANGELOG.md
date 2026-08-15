# Changelog

## v1.6.4 (current)

- **Incremental persistence — a turn reaches disk before it finishes.** The CLI
  (`appendAndSave`) and web server persist the user message up front and then
  write each message a run produces as it lands. `WebChatWorker` emits a
  `progress` signal and `WebServer::persistTurnProgress` appends whatever part
  of the running turn isn't on disk yet (tracked via `m_persistedCount`). A
  crash or cancel used to silently drop the whole turn's tool calls while the
  user message stayed on disk.
- **Mid-run renames are preserved.** `persistTurnProgress` re-reads the chat
  from disk on every write, so a `/rename` landing mid-run is no longer
  clobbered by the worker's stale copy.
- **Dangling tool calls are repaired on any run end.** The GUI
  (`onWorkerError`), CLI, and web server run `cleanDanglingToolCalls` before
  their last save, synthesizing a placeholder tool message for any orphaned
  assistant `tool_calls` so the next request does not go wrong.
- New `webPersistsTurnMidRun` test covers the web path, and existing send tests
  now wait on the assistant reply rather than just the up-front user message.

## v1.6.3

- **Fix: Stop button left the sidebar status bubble stuck.** Pressing Stop cleared
the tab's thinking/tool-running state but never refreshed the quick-settings
status dot, so the bubble stayed on "Thinking…" (blinking red) or
"Running Tool…" (orange) instead of returning to green "Idle". The Stop handler
now repaints the status bubble, matching the normal completion and error paths.
Fixed in all three editions (Python, C++, Rust).

## v1.6.2

- **Persistent model list and per-tab model selection (desktop GUI):** the sidebar
  "Model:" field is now an editable dropdown, pre-populated from a persistent model
  list cached in `~/.config/pengy/models_cache.json` (shared across the Python, Rust
  and C++ editions). Each chat tab remembers its own model — stored on the chat
  record — overriding the global default, and the dropdown follows the active tab.
  Settings → Fetch refreshes and re-persists the list; a hint appears under the
  dropdown when no cache has been fetched yet.

## v1.6.1

- **Qt local-image rendering fix:** raw HTML `<img>` tags now render canonical
  `file:///…` local URLs correctly in the desktop chat view. The loader also
  accepts absolute local paths emitted by models.

- **Tooling updates:**
  - `download_file` now streams directly to a configurable directory (default
    `~/Downloads`), returns the saved path and byte size, overwrites same-name
    files, supports per-call `max_size_mb` limits (`0` = unlimited), and uses a
    120-second no-data stall timeout so large transfers can finish.
  - `fetch_url` and `read_multiple_files` now follow the configured global tool
    output limit; `fetch_url` also accepts a `max_chars` override.
  - `run_bash` and `run_python` accept an optional `cwd` working directory.
  - `search_content` matches literal text by default; pass `regex=true` for
    regular-expression searches. Tool descriptions now document their limits,
    safety behavior, and argument semantics more precisely.
- **Tool defaults and controls:** tool execution now defaults to 300 seconds
  (matching the documented setting), and the new `download_max_mb` setting
  controls the default download cap (100 MB by default, `0` = unlimited).

## v1.6.0

- **New `read_image` tool** — the agent can inspect local images (screenshots,
  photos, diagrams, charts, rendered plots) and attach them to the conversation
  so vision-capable models can describe what they show.
  - Images decoded, preprocessed (resize/compress to configurable limits), and
    base64-encoded via Qt6 `QImage` / `QFile`
  - Parked on `ToolContext` (not the tool return value) because the API only
    accepts string content in `role: "tool"` messages
  - Attached as a follow-up user message with `image_url` parts after the tool
    loop completes
  - Added to `isReadOnly()` safe-list for auto-approval in "safe" mode
  - Limits configured via `setImageLimits()`, shared across all frontends
  - Tests: image attachment, error handling, LLM loop integration, all 3
    binaries (GUI, CLI, web) link the new tool
- **Graceful degradation for text-only models**: if the API returns HTTP 400
  because the model doesn't support vision inputs, the `image_url` parts are
  automatically stripped from all messages, a clarifying note is appended, and
  the conversation retries without the image — instead of emitting a
  `final_response` with "API error (HTTP 400)" and ending the chat.
  Implemented in all three editions (Python, C++, Rust).
- **Fix: tool output truncation now cuts on line boundaries and separates file
  reads from command output**:
  - `read_file` / `read_multiple_files`: truncate from the head only
    (contiguous, no middle gap) — the head has imports/declarations, the rest
    can be paged via `offset`
  - `run_bash` / `run_python`: remain tail-biased (head + tail, middle snipped)
    — command echo at the start, errors at the end, disposable middle
  - Both seams cut on full line boundaries so the model never sees a broken
    half-line fragment
  - Whole-file reads that fit within the limit stay bare — no `[Lines X-Y]`
    header to parse
  - Truncated file headers show the exact continuation offset for easy paging
  - Giant single-line files fall back to character-boundary cutting
  - New `ToolContext::snip()` helper and `consoleWidth()` utility
- **Updated README screenshots** — new settings, templates, and main UI images.

## v1.5.9

- Fix web SSE reconnect race: `WebServer` now keeps an append-only event log
  per chat, assigns monotonic `id:` values to SSE events, and resumes from
  `Last-Event-ID` on reconnect. A phone sleep / tab switch can no longer drop
  the `final_response` and leave the UI stuck on "Thinking…".
- Mobile web layout fixes: remove double-counted safe-area padding that made a
  gap below the input bar, let Firefox Android scroll a focused prompt above
  its software keyboard, and explicitly bring the prompt into view on focus.

## v1.5.7

- `run_bash` now authenticates sudo via `SUDO_ASKPASS` instead of piping the password to stdin — fixes sudo in pipelines (`echo x | sudo tee f`), with redirected stdin, after a command that reads stdin, and for the second and later `sudo` in one command
- Fixed `search_content` tool output limits — wasn't respecting the global snip setting
- Added missing `qt6-svg-dev` to CI workflows (Linux build fix)
- `glob` tool now auto-extracts directory prefix from patterns like `~/src/*.cpp`
- Fixed `ask_user_question` schema with proper nested object support
- `QuestionDialog` sizes to fit content instead of scrolling

## v1.5.4

- Fixed `todowrite` and `apply_changes` tool schemas so the LLM generates valid calls
- Added schema-content tests to catch this class of bug automatically

## v1.5.3

- Fixed scrollbar jumping in chat view when new content arrived
- Refreshed the UI with consistent icon set
- Harmonized output limits across tools

## v1.5.2

- Added `apply_changes` tool — multi-file transactional edits with dry-run diff preview
- Raised default tool output limit from 50 KB to 250 KB
- Harmonized `directory_tree` and `read_multiple_files` limit handling

## v1.5.1

- Added origin guard for web UI (CSRF/DNS rebinding protection) with `--trusted-host` flag
- Robust CLI argument parsing across all entry points
- Status dot in GUI sidebar shows live connection state

## v1.5.0

- **Three new tools** — `glob`, `todowrite`, `ask_user_question`
- **Tabbed chat** — multiple concurrent sessions, each with its own worker thread
- Fixed threading: `m_cancelled` is now `std::atomic<bool>` to prevent data races
- Per-tab tool context means stopping one tab won't kill another

## v1.4.5

- Context management: tool results snipped (head + tail) when they exceed the configured limit
- Prevents context window blowout from large file reads or search results

## v1.4.4

- Chat history rewritten: per-chat files + index.json for faster loading at scale
- HTML render cache turned O(n²) re-renders into O(n)
- Sidebar performance improvements

## v1.4.3

- UI audit parity with Python edition: confirmation labels, delete confirmations, auto-grow input, CLI tab completion, web sticky scroll, navbar/badge theme prep

## v1.4.2 – v1.4.1

- Performance: faster chat load and render, cleaned up old hardcoded paths

## v1.4.0

- Configurable LLM timeout setting
- Mobile-friendly web UI
- Default tool timeout bumped from 60s to 300s

## v1.3.11

- Image preprocessing for LLM vision APIs
- Web UI renders local images via `/files` route with `![alt](url)` markdown support
- Exponential backoff on 429/529 HTTP status from LLM providers

## v1.3.8 – v1.3.7

- Added binary smoke tests to build scripts
- Better 500 handling in web UI
- Bugfixes and quality-of-life improvements

## v1.3.5 – v1.3.4

- Reasoning traces displayed for models that emit them
- CLI improvements: `/show`, `/tail`, `/rename`, `/clear`, `/export` commands, better startup
- Added `--model`, `--output`, `--config-dir`, `--system`, `--no-browser` CLI flags

## v1.3.3 – v1.3.1

- Stop button in web UI, responsive web layout
- Many GUI fixes (wider dropdowns, shorter task previews, shared config path)
- Fixed Qt theme application

## v1.3.0

- **Theme system** — light/dark/system modes with accent colours, font scaling
- **Tasks** — reusable prompt templates with `%placeholder%` tokens
- Font scaling fix — markdown and code fonts now track the configured UI scale

## v1.2.x

- Reasoning effort and reasoning history options for compatible models
- CI/CD fixes for all platforms (Linux AppImage, macOS DMG, Windows MSVC)
- Fixed Windows build with conditional readline dependency
- Cross-edition documentation and interop testing
