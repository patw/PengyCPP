# Rust proactive compaction: live fast validation — 2026-10-23

Operator: Pat authorized the configured `fast` route. Provider response model: `FW-DeepSeek-V4.1-Flash`. All inputs were synthetic, all tool execution blocked, output allowance 2,048 tokens. Production settings/history were not changed. This is evidence for the tested workloads, not a lossless-summary guarantee.

## Workload and harness

- 200 completed synthetic turns, 401 source messages including the system instruction.
- Approximately 649K text characters, provider reports 248,184 prompt tokens on the first request.
- Permanent requirement in the oldest turn: `HARBOR_17`, `/tmp/harbor-17`, audit pending.
- Rust core exercised directly with `examples/live_proactive_context.rs`, driven by `tests/live_proactive_context.py`.
- A loopback recorder forwards only the authorized `fast` model to the configured proxy, keeping authentication out of artifacts. It records usage, status, request kind and hashes; all settings and transcripts are in an isolated scratch directory.
- Reproduce explicitly: `python3 tests/live_proactive_context.py --output-dir /tmp/pengyr-fast-live-<unique-name>`. This is an operator-run live test, not part of `cargo test`.

## Results

| Case | First prompt | Next prompt | Follow-up prompt | Compactions |
|---|---:|---:|---:|---|
| Configured 1,048,576-token window | 248,184 | 248,230 | — | None |
| Artificial 262,144-token Pengy budget on the same fast endpoint | 248,184 | 188,002 | 188,048 | One proactive step on turn 2; none on turn 3 |

The artificial budget tests the client policy on a real provider; it **does not** establish Strata overflow behavior or independently discover fast's actual limit. The configured proxy `/models` entry for `fast` contains no window metadata, so explicit windows were used.

The first request bootstrapped calibration without compaction. The second artificial-budget turn emitted six summary-start events, then exactly one `context_compacted` with `reason: "proactive"`, then its final answer. The existing planner summarized an oldest prefix in six chunks; 156,916 characters were removed from the provider view. Every answer retained the exact marker/path and pending status. The original transcript prefix matched exactly; no tools were attempted. All 11 requests in the successful run returned HTTP 200 and `stop`. Per-turn usage reconciled exactly with summary-plus-main request usage.

Compaction turn wall time: 22.76 seconds. Follow-up: 1.60 seconds. Successful run usage: 1,182,647 prompt tokens, of which 934,000 reported cached; 1,268 completion tokens. No price estimate is asserted.

## Finding and correction

The initial live trial exposed a bounded-summary failure: summaries omitted reasoning controls, and the second chunk consumed 1,856 reasoning tokens within its 2,048-token allowance and ended `length`. Pengy rejected the whole incomplete plan, wrote no checkpoint, sent the original provider view, and returned the correct answer. Failure artifacts were retained.

Correction: tool-free summary requests now use the **explicitly configured** endpoint/model thinking-off mechanism (`empty_reply_thinking_off` / `_models`). For fast, the scratch configuration uses `reasoning_effort`; all six summary calls then reported zero reasoning tokens and completed. Unknown/unconfigured providers retain their previous request shape. Ordinary conversation effort is not changed by summarization. A stub-server regression pins all three shapes: unconfigured, reasoning effort none, template thinking false.

After the correction: `cargo test --workspace -- --skip tools::remote_tests` passed **417 tests**, zero failures, 20 remote tests filtered. The usual existing web dead-field warning remains.

## Local artifacts and remaining work

- Initial trial: `/tmp/pengyr-fast-live-20261023/`
- Successful trial: `/tmp/pengyr-fast-live-20261023-fixed/` (`report.json`, `requests.json`, per-turn events, exact synthetic transcript)
- Regression log: `/tmp/pengyr-post-live-tests.log`
- No version bump, installation, commit, push or release performed.
- Follow-up: actual Strata 256K overflow/large-tool validation subsequently passed on the same day; see [Strata evidence](proactive-context-strata-validation.md). Python/C++ parity was subsequently ported and regression-tested; those ports were not part of this Rust live run. Long-length and reasoning-only reply retries remain stub-tested, not induced on fast in this live run.
