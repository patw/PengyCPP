# Automatic context recovery

All editions use the same provider-only policy. Default: enabled. Recovery never edits the saved message transcript or attachment objects and never re-executes completed tools.

## Triggers and stages

Recovery starts after an explicit provider context-limit error (400/413/422 with a recognized code/message), or an HTTP 200 `finish_reason: length` completion with empty/whitespace answer and **no tool calls**. The latter is only a possible context-pressure signal: a small generation cap can cause the same response. Partial answers and truncated tool calls retain the safe failure behavior from 1.11.1.

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

## Testing

- `tests/fixtures/context_recovery.json` is shared across Python/Rust/C++; it pins provider views, hashes, reasoning handling, synthetic image task boundaries, tool previews, and history summaries.
- Stub-server tests cover empty-length/context-error recovery, no tools on summary requests, unchanged history, bounded retries, partial/truncated failures, and persisted checkpoints.
- Python reference: `python -m pytest tests/test_auto_context_recovery.py tests/test_context_recovery.py tests/test_truncation.py tests/test_llm_loop.py`.
- Rust: `cargo test --workspace -- --skip tools::remote_tests` (remote mocks require a `setsid` executable on macOS).
- C++: build with CMake, run `pengy_tests` recovery cases and GUI CTest suites. Linux-only remote mocks are separately classified on macOS.
- Operator-run live scripts are under Python `tests/live_*context_recovery.py`. They use synthetic data and isolated config/history and must only be run against an explicitly authorized endpoint. They intentionally induce substantial context pressure; do not point them at a shared production provider without approval.

Live validation on 2026-10-05: Strata 0.1.38, Qwen3.8-Flash-Next IQ3_XXS, context 262144. Explicit context pressure recovered by tool preview and by history summary; exact `HARBOR_17`, `/tmp/harbor-17`, and pending audit survived summarization and a follow-up. A 262112-token prompt left only 24 generation tokens: recovery disabled produced `GenerationLimitError`, enabled recovered to `EMPTY_RECOVERY_OK`. Both native CLIs also recovered against Strata without changing saved tool output. Validation reports were written under `/tmp/pengy-strata-*-recovery*` on the development Mac. This is evidence for the tested workloads, not a guarantee of lossless summarization for arbitrary conversations.
