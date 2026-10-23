# Automatic context recovery

All editions use the same provider-only policy. Default: enabled. Recovery never edits the saved message transcript or attachment objects and never re-executes completed tools.

## Proactive compaction (all editions, opt-in)

All editions can reduce the provider view **before** a request. Existing configurations remain reactive: `context_window: 0`, an empty `context_windows` map, and `context_window_auto: false` enable no proactive work.

```json
{
  "context_window": 0,
  "context_windows": { "local": 262144, "fast": 1048576, "constrained": 131072 },
  "context_window_auto": false,
  "compact_at_percent": 75,
  "empty_reply_thinking_off_models": { "local": "reasoning_effort" }
}
```

A model-specific number wins over the global value; model-specific zero disables proactive compaction even in auto mode. A positive global value describes a **combined input/output window**. To discover a window instead, explicitly set `context_window_auto: true` with no positive override. Discovery prefers the selected model's `/models` metadata (`context_window`, `context_length`, `meta.n_ctx`, `max_input_tokens`). `/props` (`default_generation_settings.n_ctx`) and `/health` (`max_context`) are used only when the model identity is established by the response or a matching single-model list. Unsupported discovery returns unknown rather than assuming a window. A maximum-context number in an explicit context error can update the opted-in cache. Cache files are `<config>/context_windows/<sha256(route)>.json`, positive results expire after a day, negative results after five minutes. Discovery has a two-second total deadline and shorter per-request timeouts; a proxy that hides metadata requires an explicit override.

**Measurement:** pair the last valid chat request's actual sent text/schema/argument character count with positive `usage.prompt_tokens`. Estimate the next request from that measured density, rounding upward. Counts are not turn-accumulated usage, and summary requests never calibrate the main chat. There is no hardcoded chars/token divisor or model-era default. Text-only calibration is skipped when images/audio/files are in the request. Average density remains an approximation: adding dense logs to prose can underestimate growth.

**Bootstrap:** with no sample, send normally and rely on reactive recovery. The next tool round can already use reported usage. Validated per-chat/per-route calibration and hysteresis survive later user turns in the same process, but not a process restart; edited prefixes, changed instructions, provider views, window or route invalidate reused calibration. Reopening an already-large uncached chat therefore starts reactively. There is no warm-up/tokenizer request. `tool_output_max_chars` is a per-result limit, not a total-context guarantee.

**Threshold:** at 75% a 262,144-token window triggers at 196,608 prompt tokens. For combined windows, an explicit `output_token_limit` can lower that threshold to `window - output_token_limit`; input-only `max_input_tokens` discoveries do not subtract an output reserve. Output allowances at least as large as a combined window are rejected. The 65,536-token headroom is an operating margin, not enough to guarantee room for a 250,000-character log result plus generation.

**Bounds:** check once per outer request, including every tool round, and commit at most one ladder step. At most two proactive reductions per user run consume the existing four-reduction budget, leaving at least two for reactive recovery and retaining the final-summary reservation. Proactive summaries use at most eight of the sixteen summary calls. If no plan can reduce, send anyway. A failed/incomplete proactive summary leaves the checkpoint unchanged, suppresses further proactive work that run, and sends the original provider view unless cancelled. Reactive failure remains explicit.

After a reduction below threshold, the next proactive trigger requires another five percentage points of window pressure. A reduction still above threshold may be followed by another step on a subsequent request; reaching the hard input budget bypasses hysteresis. Reactive errors never consult hysteresis. Transcript and protected-turn rules are unchanged. The existing event gains optional `reason: "proactive"` with a message such as “Compacting before the window fills”; legacy reactive events omit that field.

## Bounded reply recovery (all editions)

In addition to the existing empty/short `length` heuristic, an unbounded-output request (`output_token_limit: 0`) gets **one additional reduction retry per run** for a long/unreported length completion. A provider can still impose its own output cap, so a second long length stop fails explicitly. Truncated replies are never saved or executed, and completed tools are not replayed. Explicit output caps retain the existing policy.

An empty answer with nonempty `reasoning`/`reasoning_content`, `finish_reason: stop`, and no tool calls can retry **once per run** when a supported mechanism is configured. `empty_reply_thinking_off` is `""` by default (disabled), `"reasoning_effort"` sends `reasoning_effort: "none"`, and `"enable_thinking"` sends `chat_template_kwargs: {"enable_thinking": false}`. `empty_reply_thinking_off_models` overrides that choice per model, including an empty string to disable it. Only configure a mechanism your endpoint accepts; generic providers receive no guessed parameter. Bounded tool-free summary requests also use that explicitly configured mechanism to keep their 2,048-token allowance available for the checkpoint rather than hidden reasoning. Ordinary conversation requests retain the user’s selected effort unless the empty-reply retry is used. The setting and original request history are not mutated, discarded usage is accounted, and a second empty reply is an error.

Python, Rust and C++ implement the same opt-in settings, estimator/bootstrap, budgets, hysteresis, discovery cache and bounded reply recovery. The new native editions preserve unknown shared settings as Python already does. Older native binaries can still erase these settings: update every edition before mixed-edition use. The provider-view checkpoint and route-window cache formats are shared; calibration itself is process-local and is not transferred between running editions.

## Triggers and stages

Recovery starts after an explicit provider context-limit error (400/413/422 with a recognized code/message), or an HTTP 200 `finish_reason: length` completion that is either an empty/whitespace answer with **no tool calls**, or a partial answer/truncated tool call whose reported `completion_tokens` is below 1,024 (and below `output_token_limit` when set). Length stops are only a possible context-pressure signal: a small generation cap can cause the same response. A completion that short is not a plausible output cap — it is typically a lead-in sentence cut off before its tool call because the window was nearly full. The truncated reply is discarded unexecuted and regenerated after reduction. Longer or unreported completions retain the safe failure behavior from 1.11.1, and every length failure reports the prompt/completion token counts so a cap can be told apart from context exhaustion.

Each retry must make the provider view smaller. The per-user-run limit is **four reduction/retry attempts**, shared across tool rounds:

**Attempt 4 reserves a chance for history summarization.** If an eligible completed-turn summary fits the remaining summary-call budget, it takes priority over additional reasoning/tool reductions on the final attempt. Otherwise tool compaction can still use that attempt. A failed, incomplete, or non-reducing selected summary does not trigger a destructive fallback or extra retry. Attempts 1–3 keep the normal order below; summarization can also happen earlier when no reasoning/tool reductions remain.

1. Omit historical `reasoning` / `reasoning_content` from completed tasks. Keep the active task's reasoning and all `reasoning_details`, which may contain provider-required signatures or opaque continuation state.
2. Replace large tool-result bodies with 1,500-character head/tail previews, then omission stubs if needed. Preserve assistant tool-call structure and matching result IDs. Prefer older results; the latest result is reduced only when no older eligible result remains.
3. Summarize whole oldest completed turns. Preserve system/developer instructions, the current user task and its tool chain, and the configured recent completed turns. `read_image` synthetic multimodal follow-ups are not new task boundaries.

Summaries are separate, tool-free requests to the same selected model/endpoint. They cover the selected source in ordered chunks of at most 32,000 characters, using a 2,048-output-token allowance per chunk and a **16-summary-request limit** per run. The previous checkpoint is included when extending a summary. Empty/truncated/failed/non-reducing summaries stop recovery without committing that plan. There is no silent dropping fallback.

The summary prompt asks to retain user requirements, exact names/paths/numbers, decisions, completed actions/outcomes, and pending work. Historical material is sent back as a clearly labelled user-level checkpoint, **never elevated to system/developer instructions**. Summaries can still lose detail or be influenced by source material; preserving the original history does not guarantee that the model remembers everything. Check important requirements, and ask for missing details rather than inventing them.

## Shared configuration

These settings are in `settings.json`; the current UI need not expose a new control to edit them:

| Setting | Default | Behavior |
|---|---|---|
| `auto_context_recovery` | `true` | Enable the new recovery policy. `false` retains the previous explicit-error tool-output-only recovery and truncation failures. |
| `recovery_keep_turns` | `3` | Recent completed user tasks protected from history summarization, in addition to the active task. |
| `output_token_limit` | `0` | Zero omits the output limit. Positive values reserve an explicit requested generation allowance where the provider supports it. |
| `output_token_parameter` | `"max_tokens"` | Either `max_tokens` or `max_completion_tokens`; no automatic fallback silently changes provider semantics. Also selects the summary request parameter. |

Do not set an output allowance larger than the model's context window. Strata's `fit_max_tokens` may clamp the allowance rather than reject insufficient room; empty-length recovery remains available. A reasoning budget is a separate provider-specific policy and is not automatically changed.

## Durable checkpoint and privacy

Reduction plans are stored separately at:

```text
<config>/context_recovery/<sha256(chat-id)>.json
```

The plan stores an endpoint/model/instruction identity, per-message prefix hashes, summarized-prefix boundary, summary text, and tool-reduction stages. It stores no image data or opaque reasoning envelopes. It is applied to provider request copies for later tool rounds and subsequent turns. Changed prefix content, invalid turn boundaries, model/endpoint/instruction changes, or corrupt checkpoints invalidate the plan. Chat titles and usage do not affect it. Disabling recovery ignores the sidecar; deleting the sidecar resets the view. `--no-save` does not write a sidecar. Original chats remain viewable/exportable, and attachment objects are untouched. Sidecar summaries are sensitive local chat data, just like the transcript.

Recovery notices are delivered through `context_compacted` with an explanatory `message` and shown in desktop, CLI stderr, and web. Summary-call usage and discarded empty-length completion usage are included in the eventual successful turn usage. No recovery inference is reported as a completed assistant answer. If the protected task cannot fit, the run fails explicitly; it never discards the active user request to force success.

## Live proactive validation

2026-10-23, operator-authorized `fast` (`FW-DeepSeek-V4.1-Flash`), 200 synthetic turns: a configured 1M window did not compact approximately 248K-token prompts. The same endpoint with an artificial 256K client budget bootstrapped normally, then committed one proactive summary and reduced the next prompt to 188,002 tokens; a follow-up at 188,048 did not compact. Exact marker/path/status survived and the transcript was unchanged. The initial trial exposed reasoning exhausting a bounded summary allowance; using the configured thinking-off mechanism for summary calls fixed it. [Full fast evidence and caveats](proactive-context-live-validation.md).

The same day, actual Strata 0.1.41 / Swift-Qwen 3.8 at 262,144 passed: a 218,932-token history prompt was proactively reduced to 166,643; a 249,093-character tool result was retained in the transcript but previewed before the next request. The identical unreduced tool-chain view was separately rejected at 405,835 tokens, proving a real overflow was avoided. Rust window discovery returned the live 262,144 value. [Strata evidence, prefill timings and caveats](proactive-context-strata-validation.md).

## Testing

- `tests/fixtures/context_pressure.json` pins measured log/prose density arithmetic for current 128K/256K/1M workloads; loop tests also cover cold starts, later user turns, unknown usage, hysteresis/budgets, failed proactive plans, reactive 400s, and bounded reply recovery.
- `tests/fixtures/context_recovery.json` is shared across Python/Rust/C++; it pins provider views, hashes, reasoning handling, synthetic image task boundaries, tool previews, and history summaries.
- Stub-server tests cover empty-length/context-error recovery, no tools on summary requests, unchanged history, bounded retries, partial/truncated failures, and persisted checkpoints.
- Python reference: `python -m pytest tests/test_auto_context_recovery.py tests/test_context_recovery.py tests/test_truncation.py tests/test_llm_loop.py`.
- Rust: `cargo test --workspace -- --skip tools::remote_tests` (remote mocks require a `setsid` executable on macOS).
- C++: build with CMake, run `pengy_tests` recovery cases and GUI CTest suites. Linux-only remote mocks are separately classified on macOS.
- Operator-run live scripts are under Python `tests/live_*context_recovery.py`. They use synthetic data and isolated config/history and must only be run against an explicitly authorized endpoint. They intentionally induce substantial context pressure; do not point them at a shared production provider without approval.

Live validation on 2026-10-05: Strata 0.1.38, Qwen3.8-Flash-Next IQ3_XXS, context 262144. Explicit context pressure recovered by tool preview and by history summary; exact `HARBOR_17`, `/tmp/harbor-17`, and pending audit survived summarization and a follow-up. A 262112-token prompt left only 24 generation tokens: recovery disabled produced `GenerationLimitError`, enabled recovered to `EMPTY_RECOVERY_OK`. Both native CLIs also recovered against Strata without changing saved tool output. Validation reports were written under `/tmp/pengy-strata-*-recovery*` on the development Mac. This is evidence for the tested workloads, not a guarantee of lossless summarization for arbitrary conversations.
