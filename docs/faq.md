# FAQ

## General

### What's the difference between Pengy, PengyR, and PengyCPP?

They're three implementations of the same agent, sharing the same config, chat history, and skills.

| Edition | Language | Best for |
|---------|----------|----------|
| **Pengy** | Python | Hacking on the code, quick experiments, reference |
| **PengyR** | Rust + Qt6 | A single ~13 MB statically-linked binary, higher performance |
| **PengyCPP** | C++17 + Qt6 | Smallest memory footprint, zero dependencies beyond Qt6 |

All three can be used side by side — chats created in one load in the others.

### Which models does PengyCPP work with?

Any OpenAI-compatible API. See [docs/api-compatibility.md](api-compatibility.md) for a full list.

### Can I use PengyCPP offline?

Yes, with a local model. Ollama, LM Studio, or vLLM running on the same machine works great.

### Does PengyCPP phone home?

No. PengyCPP connects only to the API endpoint you configure. No telemetry, no accounts, no cloud.

## Setup

### How do I switch models?

**GUI:** Click ⚙ → change the Model field → click Fetch Models.  
**CLI:** `/model <name>`.  
**Web:** Click ⚙ → change the Model field.

### Where is my config stored?

`~/.config/pengy/settings.json` — shared with Python Pengy and PengyR.

### How do I reset everything?

```bash
rm -rf ~/.config/pengy
```

## Tools & Skills

See the [Pengy FAQ](../Pengy/docs/faq.md) for tool and skill questions — answers are identical across all editions.

## Troubleshooting

### On Windows the app was force-closed and vanished from the Start menu

Windows Defender almost certainly quarantined it. Pengy hands the model shell,
PowerShell and file-write access, and Defender's heuristics act on that: the process
is terminated without a dialog and `pengy.exe` is quarantined, which also breaks the
Start Menu shortcut. A plain reinstall does not help — the new copy is caught the
same way within seconds.

Exclude `%LOCALAPPDATA%\Programs\PengyCPP` (and the process `pengy.exe`) **first**,
then restore the file from Protection history or reinstall. Confirm it was Defender
and follow the whole procedure in
[Windows Defender and antivirus](windows-antivirus.md). The same page covers the
portable ZIP, the "Windows protected your PC" prompt and Smart App Control.

### I get 403 errors from the web UI

If you're behind a reverse proxy, you need `--trusted-host`. See [docs/reverse-proxy.md](reverse-proxy.md).

### The GUI crashes on startup

Make sure you have Qt 6.4+ installed. On Linux: `apt install qt6-base-dev`. On macOS: `brew install qt@6`.

### The LLM keeps timing out

Increase `tool_timeout` in settings.json. The default is 300 seconds.

### My API key isn't working

- Check that `base_url` matches your provider
- For local endpoints (Ollama, LM Studio), the API key can be anything
