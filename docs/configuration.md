# Configuration

PengyCPP stores its settings in `~/.config/pengy/settings.json`. This file is shared across all three interfaces (GUI, CLI, Web) and all three editions (Python, Rust, C++).

## Core settings

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `base_url` | string | `http://127.0.0.1:11434/v1` | API endpoint for your LLM provider (a local Ollama server by default) |
| `api_key` | string | `""` | API key (stored in plaintext). A local server needs none |
| `model` | string | `""` | Model name to use. Deliberately empty by default: a local server ships no model of its own, and Pengy asks you to choose one instead of sending an empty name |
| `system_message` | string | *(see below)* | System prompt with `{date}`, `{username}`, `{hostname}`, `{osinfo}` placeholders |
| `tool_confirmation` | string | `none` | `all` (auto-approve everything), `safe` (auto-approve read-only tools), `none` (confirm every call) |

## Advanced settings

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `reasoning_effort` | string | `""` | For models that support it: `none`, `low`, `medium`, `high`, `xhigh`, `max`. Empty string uses the provider default. |
| `preserve_reasoning` | bool | `false` | When true, keeps the model's raw reasoning/chain-of-thought in the chat history. |
| `tool_timeout` | int | `300` | Max seconds a tool can run before being killed. `-1` = no timeout. |
| `tool_output_max_chars` | int | `250000` | Max characters in tool output before head+tail snipping kicks in. `0` = no limit. |
| `max_tool_calls_per_turn` | int | `25` | Max tool calls the LLM can make in a single turn before PengyCPP forces a response. |
| `ui_scale` | int | `100` | GUI only. UI scale percentage (75, 100, 125, 150, 175, 200). Restart required for full native-widget scaling. |
| `theme_mode` | string | `system` | GUI only. `system` (follow OS), `light`, or `dark`. |
| `theme_accent` | string | `default` | GUI only. `default`, `blue`, `teal`, `green`, `orange`, `red`, `pink`, or `purple`. |

## Web-only settings

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `web_port` | int | `5000` | Port for the web UI |
| `web_host` | string | `127.0.0.1` | Bind address for the web UI |
| `trusted_hosts` | string[] | `[]` | Additional hostnames to accept for CSRF/DNS-rebinding checks. Set via `--trusted-host` CLI flag. |

## Default system message

```text
You are Pengy, a helpful AI agent. You have tools and skills.
The current date is {date} and the user is {username} on host
{hostname} which is {osinfo}. WARNING: ALWAYS look at
~/Personal/skills/skill_index.md before running web search,
bash, running code or url fetch! Skills should ALWAYS be used
over tools!
```

## Config file location

| Platform | Path |
|----------|------|
| Linux | `~/.config/pengy/settings.json` |
| macOS | `~/Library/Application Support/pengy/settings.json` |
| Windows | `%APPDATA%\pengy\settings.json` |

## How to edit

**GUI:** Click the ⚙ button in the sidebar. Changes apply immediately.

**CLI:** Use `/config` to view current settings, `/model <name>` to switch models. Edit `settings.json` directly for advanced fields.

**Web:** Click the ⚙ button in the top-right navbar. Changes apply immediately.

All three interfaces read from and write to the same file. You can switch between them freely — your settings follow you.

### Desktop per-chat controls

The GUI sidebar’s **Effort** dropdown overrides `reasoning_effort` for that chat only and applies to the next message. **Global setting** follows the saved default; **Provider default** explicitly sends no reasoning hint. Offered levels are Off / none, Low, Medium, High, Extra high, and Max; support depends on the model/provider. Minimal is no longer offered, though legacy raw configuration remains readable.

**Last: … tok/s** is the final API response’s output tokens divided by its successful request wall time. It includes network latency, prompt processing, and reasoning, but excludes earlier tool calls, tool execution, confirmation waits, and retry backoff. It is not pure decoding speed. Missing usage shows `—`; the last measurement is saved with the chat, separately from cumulative token totals.
