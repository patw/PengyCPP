# Skills Guide

Skills are how you teach Pengy to do things its built-in tools don't cover. A skill is just a markdown file (with an optional script) in a directory. No SDK, no manifest, no packaging.

*New to skills? Start with the [skills/README.md](../skills/README.md) tutorial first.*

---

## Skill patterns

Looking through the existing skills, a few patterns emerge:

### Pure markdown — reference information

The skill is just a markdown file. No scripts. Pengy reads it and learns.

**Examples:** `pengy_bio/`, `user_profile/`, `network/`, `news/`, `reddit/`, `clip/`, `moofile/`

Best for: teaching Pengy about your world — who you are, what your machines are, how your network is laid out, what RSS feeds you read.

### Markdown + shell wrapper — system commands

The markdown documents a CLI command, and a thin script wraps it with consistent arguments.

**Examples:** `tts/` (spd-say), `git/` (git/gh commands)

Best for: any CLI tool you use regularly. The markdown is the manual; the script is just a convenience.

### Markdown + Python script — data processing

The script takes structured input and produces an artifact.

**Examples:** `plot/` (charts → PNGs), `pdf_reader/` (PDFs → text), `pptx/` (slides → .pptx), `rss/` (feeds → JSON)

Best for: anything that transforms data. The markdown documents the interface; the script does the work.

### Markdown + Python script + API key — external services

Like the above, but needs credentials. Uses the `~/.secrets` pattern for key storage.

**Examples:** `weather/` (Tomorrow.io), `email/` (Gmail SMTP), `pengyshare/` (img.catbee.ca), `youtube_transcript/` (YouTube API)

Best for: web APIs. The `~/.secrets` file keeps keys out of scripts and version control.

### Multi-skill orchestration — combining skills

One skill calls other skills to produce a compound result.

**Examples:** `daily_briefing/` (weather + RSS + news), `podcast/` (TTS + image_gen + music)

Best for: complex workflows. Document the dependency chain in the `_skill.md` so Pengy knows what to invoke.

### GPU-accelerated — ML models

The skill runs a machine learning model locally, often requiring specific hardware.

**Examples:** `music/` (ACE-Step 1.5 DiT on CUDA), `upscale/` (Real-ESRGAN via ncnn-vulkan), `image_gen/` (Gemini flash-image)

Best for: local AI workloads. Document VRAM requirements and model paths clearly.

---

## The `~/.secrets` pattern

Many skills need API keys. The convention is a flat key-value file at `~/.secrets`:

```bash
# ~/.secrets
TOMORROW_IO_KEY=your_key_here
GMAIL_APP_KEY=your_app_password
PENGYSHARE_KEY=your_upload_key
```

Protect it:

```bash
chmod 600 ~/.secrets
```

Scripts read it by sourcing or parsing. The `weather` skill's `get_weather_by_location.py` shows the canonical pattern:

```python
def _read_secrets():
    secrets = {}
    secret_file = Path.home() / ".secrets"
    if secret_file.exists():
        for line in secret_file.read_text().splitlines():
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                secrets[k.strip()] = v.strip()
    return secrets
```

---

## The `uv` dependency pattern

Python skills that need packages use [PEP 723](https://peps.python.org/pep-0723/) inline script metadata. Add a `#!/usr/bin/env -S uv run` shebang and a `/// script` block:

```python
#!/usr/bin/env -S uv run
# /// script
# dependencies = ["matplotlib"]
# ///
```

When Pengy runs the script, `uv` auto-creates a venv and installs dependencies. First run is slow; subsequent runs are cached. See `plot/make_plot.py` for a working example.

For heavier dependencies (like the `tts` skill with Kokoro), use a dedicated venv:

```bash
uv venv ~/skills/tts/.venv
uv pip install --python ~/skills/tts/.venv/bin/python kokoro misaki
```

The `_skill.md` documents which venv to activate.

---

## The skill index

`skill_index.md` is Pengy's table of contents. Format:

```markdown
| Dir | What | Python deps via uv? |
|-----|------|:---------------------:|
| weather/ | Fetch weather from tomorrow.io | ✅ |
| tts/ | Text-to-speech on Ubuntu | ✅ (uv venv) |
| git/ | Git & GitHub CLI helper | ❌ (stdlib only) |
| network/ | Home network inventory | ❌ (no script) |
```

The third column (Python deps) is optional but helps Pengy know whether it needs to `uv run` or just `python` the script.

---

## Writing effective `_skill.md` files

### Do

- **Start with a one-line summary** of what the skill does
- **Document the exact command syntax** Pengy should use, with a table of arguments
- **Include examples** — Pengy learns from examples
- **Note dependencies** — API keys, Python packages, system tools
- **Document error handling** — what happens if the API is down, rate limited, etc.

### Don't

- Don't describe *how* the script works internally — Pengy doesn't read scripts, only the markdown
- Don't assume Pengy knows your file paths or API keys — spell everything out
- Don't use vague language like "use the appropriate flags" — be specific

### Template

```markdown
# Skill Name

One-line summary of what this skill does.

## Usage

python script.py <arg1> [--flag VALUE]

| Arg | Required | Description |
|-----|----------|-------------|
| arg1 | ✅ | What this argument is |
| --flag | ❌ | Optional flag (default: false) |

## Dependencies

- **API key:** `SOME_KEY` in ~/.secrets
- **System:** ffmpeg, curl
- **Python:** none (stdlib only)

## Examples

python script.py "hello world"
python script.py --flag debug "hello world"

## Error handling

- Missing API key: prints "Error: API key not found"
- Network timeout: retries once after 5 seconds
```

---

## Testing skills

After creating a skill, test it by asking Pengy to use it:

```
> Use the weather skill to get today's forecast for Toronto
```

If Pengy gets the syntax wrong, tweak the `_skill.md` — make the instructions more explicit, add more examples, or simplify the argument structure. The markdown is the contract; iterate on it until Pengy gets it right every time.

For script-level testing, run the script directly:

```bash
python ~/skills/weather/get_weather_by_location.py 43.653 -79.383
```

This separates "is the script working?" from "is Pengy calling it correctly?"
