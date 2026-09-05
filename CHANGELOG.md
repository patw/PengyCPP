# Changelog

## v1.8.1

- **CLI: sanitize ANSI/control chars in tool & error display.** Untrusted
  tool/compiler output was echoed to the terminal verbatim, so ANSI escape
  sequences could move the cursor, clear the screen, or corrupt the line layout.
  A shared `sanitizeDisplay()` (`cli/sanitize.h`) now strips CSI/OSC/DCS/single-byte
  escapes and non-`\n`/`\t` C0/DEL control chars from the tool-result,
  tool-request, and error-display paths. Display-only: raw bytes to the model are
  unchanged, and emoji (UTF-16 surrogate pairs) pass through untouched. Covered by
  `sanitizeDisplayStripsAnsiAndControl` in `tests.cpp`.
- **About window & page.** A new `About` tab in the Settings dialog and an `/about`
  web route — backed by a shared `about.h` — show the edition name + version, a
  short description, the project repo/website/license links, and a Catbee
  attribution, consistent across the CLI, GUI, and web.

## v1.8.0

- **Durable, content-addressed attachment storage** (attachment schema v1):
  chat JSON now stores small `sha256:` references; validated source bytes and
  bounded image derivatives live under `<config>/attachments/objects|derivatives`,
  written atomically and never stored as base64/data URLs.
- **Attachment rendering across the CLI, desktop GUI, and web UI.** CLI shows
  `[image: …]`/`[attachment: …]` labels and a read-only `/attachments` storage
  report; GUI and web render image thumbnails from a new
  `/attachments/<digest>/<derivative>` route, with an "attachment unavailable"
  placeholder for missing or unknown kinds.
- **Provider message handling improvements.** Provider data URLs are derived
  transiently only during request assembly, and only for the most recent
  `attachment_context_keep_turns` user turns (new config option, default 4).
  Legacy `[Image: filename]` strings remain plain text and are never inferred as
  attachment records.
- **Web chat export** now supports self-contained HTML and ZIP bundles
  (`/chat/<id>/export/html`, `/export/zip`) in addition to Markdown; both embed
  attachment images instead of referencing external paths.
- **Expanded attachment tests** (storage layout, missing/unknown kinds, legacy
  placeholders, markdown/html/zip exports).

## v1.7.3

- **Safer sudo handling.** `run_bash` now requires explicit `elevated=true` before invoking sudo, ignores sudo mentions in quotes/comments/data, and scopes cached sudo credentials to each web worker. Rust and C++ editions flush tool output before the hidden-password prompt for reliable terminal ordering.
- **Sudo password no longer sits in the child environment.** The `SUDO_ASKPASS` helper no longer receives the password through the environment; it's written to a private `0600` file that only the `0700` askpass script reads, so grandchildren / `printenv` can't observe it. Single-use and cleaned up on exit.
- **Binary guard for file/URL tool reads.** `read_file`, `read_multiple_files`, and `fetch_url` now reject content that decodes as UTF-8 but is actually binary (e.g. UTF-16 NUL-interleaved text) that would flood context, on top of the v1.7.2 command-output guard.
- **The LLM "retrying" (429/529) backoff event is now surfaced.** The CLI, Qt GUI, and web UI show it instead of hanging silently.
- **Delete-chat hygiene.** Deleting a chat now also trims it out of the legacy `chats.json` seed, so a later legacy re-import can't silently resurrect it.
- Tests added/updated for all of the above (including the resolve-attachments missing-path warning).

## v1.7.2

Ported from the Python and Rust editions, restoring feature parity across all
three editions.

- **Docs fix.** SPEC.md corrected to document the `read_image` tool (already
  implemented in `tools.cpp`, but missing from the tools table and tool-count
  references, which undercounted 15 instead of 16) and to list it as
  read-only in the `isReadOnly()` summary.
- **Binary output guard.** `snipMiddle()` (the shared truncation point for
  `run_bash`, `run_python`, `directory_tree`, `search_content`, and `glob`)
  now runs a `looksBinary()` heuristic first: a NUL byte anywhere in the
  first 4KB, or a non-printable/control-char ratio over ~25%, blocks the
  output outright with a short diagnostic instead of loading it into
  context. `readAndRemove()`'s `QString::fromUtf8()` was already lenient
  (substitutes U+FFFD for invalid sequences rather than failing), so this
  edition never had the "invalid UTF-8 silently vanishes" bug the Python and
  Rust editions had — only the "valid-but-garbage" case needed a new guard.
- **Redact last message.** `messagesRedactLast()` pops exactly one raw
  message off the end of a chat per call — a tool result, an assistant
  `tool_calls` request, or a final response — repeatable all the way to an
  empty chat. A popped tool result strikes its id directly from the
  assistant's `tool_calls` array rather than falling through to
  `cleanDanglingToolCalls()`'s "cancelled" synthesis, which would regenerate
  an identical stub forever and never let redaction advance. Wired as
  `/redact [n]` in the CLI, a redact button in the Web navbar (`POST
  /chat/:id/redact`, refused with 409 while a turn is in flight), and a
  "Redact" button in the GUI input row.
- **Tasks in the CLI and Web UI.** Previously GUI-only (`taskmanager.cpp`
  wasn't even compiled into the `pengy-cli`/`pengy-web` binaries); `/tasks`
  and `/task <n>` in the CLI, and a Tasks modal (`GET /tasks`, `POST
  /tasks/render`) in the Web UI, both routing the rendered prompt through the
  normal send path.
- **Cumulative token usage.** `chatAddUsage()` accumulates each turn's token
  counts into `chat["usage"]` (persisted, not session-only state), so the
  running total for a chat survives reloads and tab switches instead of only
  ever showing the last turn's numbers. All three frontends show it next to
  the model/tool-confirmation status.
- **GUI: "New Chat" sidebar performance.** Two stacked costs scaling with
  total chat count made "New Chat" visibly slow with more than a couple dozen
  chats: `pengyIcon()` rebuilt a 15-pixmap `QIcon` from scratch on every call
  even though every sidebar row requests the same `(name, color)` (fixed with
  a cache), and `createNewChat()` called `loadChats()`'s full
  clear-and-rebuild on every click (fixed with `ChatHistoryWidget::addChat()`,
  a single-row insert). Fixing the full rebuild uncovered a real regression:
  `closeTab()`/`loadIntoNewTab()` delete an abandoned empty "New Chat" from
  disk but never removed its sidebar row, previously masked by the full
  rebuild that ran right after — without it, closing an empty chat and
  clicking New Chat again left a permanent ghost row each time. Fixed with
  the matching `ChatHistoryWidget::removeChat()`.
- **GUI: quick-settings whitespace gap.** The "no cached model list" hint
  label was only text-cleared once populated, not hidden — an empty `QLabel`
  still claims a line of layout height, leaving a permanent gap above "Tool
  Confirm:". Now hidden outright when a model list exists.
- **Settings: two more UI scale options.** 110% and 135% added alongside the
  existing 75/100/125/150/175/200% steps.

## v1.7.0

- **Ask the user a question, interactively.** The CLI now prompts for each
  `ask_user_question`: pick a numbered option, type your own free-text answer,
  or enter `c` to cancel (a cancel reports an empty list to the harness). The
  web UI surfaces it in an interactive modal (option indices, a free-text
  "Other" field, submit/cancel) answered through a new `POST /chat/<id>/answer`
  route. `LlmClient::run` now takes `onQuestion` as a required argument — a
  frontend with no way to ask returns empty (a cancel) instead of selecting a
  default and crashing the first time the model asks.
- **Narration now renders before the tool cards.** The text the model writes
  alongside its tool calls is persisted but was dropped from the live run — and
  the reload path put it *after* the tool cards. CLI, desktop GUI, and web now
  render it live, and the reload path renders it first (shared
  `assistantDisplayMessage` helper).
- **`PENGY_CONFIG_DIR` for built binaries.** Anything driving a built pengy
  binary can now point it at a scratch config instead of silently using the
  real settings (and API key). It sits between the explicit `--config-dir`
  override and the default `~/.config/pengy`; a leading `~` is expanded, matching
  the Python and Rust editions.
- **Web hardening:** tool cards are de-duplicated on SSE reconnect, and
  attribute content is escaped so model-supplied text can't break out of
  `title="…"`.

## v1.6.4

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
