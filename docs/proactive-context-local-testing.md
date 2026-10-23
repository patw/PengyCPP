# Testing proactive compaction locally

All editions use the same settings and checkpoint/cache formats. The feature is **off by default**. Build/run from the feature branch `feat/proactive-context-compaction`; installed release apps do not acquire the feature merely because source was pulled. No version/tag bump is part of this branch.

## Safe settings

For the existing proxy aliases, add this fragment to the test config (retain your endpoint/key/model settings):

```json
{
  "context_window": 0,
  "context_windows": { "local": 262144, "fast": 1048576 },
  "context_window_auto": false,
  "compact_at_percent": 75,
  "empty_reply_thinking_off_models": { "local": "reasoning_effort", "fast": "reasoning_effort" }
}
```

For Strata directly, select its current `/v1/models` ID (`swift-1.5-iq3_xxs` in the tested deployment), set `context_window: 262144`, and configure that model's thinking-off mechanism. Alternatively explicitly enable `context_window_auto: true`, with no override, to discover Strata's metadata. Proxy aliases may hide window metadata, so explicit overrides are more predictable there. Only configure thinking-off parameters your endpoint supports.

Windows are per-model overrides within the selected endpoint configuration; do not reuse a numeric global window when switching between a 256K and 1M model. A model-specific zero disables proactive work for that model even when auto discovery is enabled.

## Build and run on this Mac

Use separate scratch settings while comparing editions:

```bash
mkdir -p /tmp/pengy-context-test
# Put a settings.json with the desired endpoint/model and settings fragment here.
export PENGY_CONFIG_DIR=/tmp/pengy-context-test
```

Python:

```bash
cd ~/dev/Pengy
uv sync
uv run pengy-cli          # desktop: uv run pengy; web: uv run pengy-web
```

Rust CLI/web:

```bash
cd ~/dev/PengyR
cargo build --workspace
./target/debug/pengy-cli  # web: ./target/debug/pengy-web
```

Rust desktop (use a separate debug GUI build so the existing packaged app is not overwritten):

```bash
cd ~/dev/PengyR
cmake -S gui -B gui/build_context_debug -DRUST_TARGET_DIR="$PWD/target/debug" \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build gui/build_context_debug -j 4
./gui/build_context_debug/pengy
```

C++:

```bash
cd ~/dev/PengyCPP
cmake -S . -B build
cmake --build build -j 4
./build/pengy-cli         # desktop: ./build/pengy; web: ./build/pengy-web
```

Set distinct web ports if comparing multiple web servers. Do not open the same saved chat simultaneously in several editions while testing; checkpoint/cache formats are compatible, but concurrent transcript edits are still undesirable. Avoid old native releases saving the new settings.

## What to expect

- First uncalibrated request sends normally. Its reported usage calibrates the next request, including the next tool round. Restarting an edition restarts calibration, but a valid committed provider-view checkpoint still loads.
- At 75% of 262,144, estimated prompt pressure triggers at 196,608 tokens, or earlier if an explicit generation reserve requires it.
- A notice says **“Compacting before the window fills”**, with `context_compacted.reason=proactive` in the event stream.
- Only the provider view changes; original messages and tool-result bodies remain in chat/export.
- One ladder step per request, at most two proactive steps per user run, with reactive recovery still available.
- A summary can be slow on Strata: the tested history compaction took about 123 seconds including two summaries and cold re-prefill. Subsequent cached requests were about a second. This is not a UI hang; summary events should be visible.
- The same approximately 200K-token workload should not compact on fast's explicit 1M budget.
- An empty answer containing only reasoning gets one thinking-off retry only when configured. A long `length` reply without a requested output cap gets one additional reduction attempt, not unlimited retries.

To reset the provider view in a **scratch** config, remove its `context_recovery` directory; to reset discovered windows, remove its `context_windows` directory. No saved transcript needs deleting. Full policy is in [context recovery](context-recovery.md), with Rust live evidence linked there. Python/C++ ports are regression-tested; the previous live evidence was obtained through the Rust core.
