# Rust proactive compaction: live Strata validation — 2026-10-23

Pat explicitly authorized `http://10.0.23.6:8086/v1`. Synthetic input only, isolated configuration/history, and one allowlisted local `read_file` against a generated test file. No other tool was permitted. No production settings, chats, binaries or services were changed.

## Endpoint verification

- Strata **0.1.41**, loaded model `swift-1.5-iq3_xxs`.
- `/props` model path identifies Swift/Qwen 3.8 Flash Next GSQ RCO IQ3_XXS.
- `/health.max_context`, `/props.default_generation_settings.n_ctx`, and selected-model `/v1/models.meta.n_ctx` all report **262,144**.
- Rust opt-in discovery independently retrieved 262,144 from the live endpoint and wrote only the isolated cache.
- A small request verified `reasoning_effort: "none"` produces a normal answer. The test config explicitly enables that supported mechanism for bounded summaries.
- `/tokenize` and `/apply-template` return 404. Calibration uses actual completion-response usage, not a token-count service or a hardcoded divisor.

## History test

200 completed synthetic turns / 401 source messages. A numeric pilot measured **78,030 prompt tokens** for approximately 78K characters. The same composition was sized from that measurement, not from DeepSeek's density.

| Request | Prompt tokens | Proactive steps | Result |
|---|---:|---:|---|
| First uncalibrated request | 218,932 | 0 | Correct requirement line; HTTP 200 |
| Next user turn | 166,643 (main request) | 1 | Two complete tool-free summaries, checkpoint committed, correct answer; HTTP 200 |
| Follow-up | 166,704 | 0 | Correct answer; HTTP 200 |

Every answer retained `HARBOR_17`, `/tmp/harbor-17`, and audit pending. Exact original transcript prefix was unchanged. Summary plus main-call usage reconciled with reported successful turn usage. No context-limit response occurred in the normal flow.

First request wall time: **99.33s** (99.14s provider request). Compaction turn: **122.89s**, including two summary requests of 17.70s and 29.22s, followed by a 75.72s reduced-context prefill. Cached follow-up: **1.20s** (1.00s provider request). This demonstrates the real cold-prefill cost on this backend; fast's much shorter times must not be generalized to Strata.

## Large tool result inside one turn

A separate 200-turn synthetic history started below threshold at **159,180 prompt tokens**. The model requested one local read of the generated synthetic file. The harness approved only that exact file and cancelled any other tool path/name.

- Full tool result retained in the event/transcript: **249,093 characters**, including its ranged-read header.
- Event order: `assistant_tool_calls`, `tool_request`, `tool_result`, `context_compacted(reason=proactive)`, `final_response`.
- Exactly **one tool execution**; no tool replay.
- Provider copy reduced to a **3,082-character** head/tail preview; 246,011 characters removed.
- Next provider request: **162,191 prompt tokens**, HTTP 200, complete correct answer.
- Follow-up: **162,249 prompt tokens**, HTTP 200, no further compaction.
- Complete turn wall time: **74.44s**, mostly its first 70.17s prefill; post-tool answer request 3.90s. Follow-up 1.08s.

**Real overflow counterfactual:** sent the exact original, unreduced tool-chain view separately with a 64-token output cap. Strata returned HTTP 400:

```text
prompt (405835 tokens) leaves no room to answer in the context (262144); requests are never truncated
```

Thus the proactive pre-send check prevented a demonstrated real overflow for this workload, not merely an artificial client budget. A separate oversized numeric prompt was also refused at 1,040,030 tokens with the same explicit non-truncation contract. No expensive inference was performed for either rejection.

## Changes and regression status

No production recovery behavior needed repair during the main Strata scenarios. Live harness adaptations added explicit model manifests, the single safe read-file approval, provider-measured sizing, and discovery/overflow checks. An initial harness build typo was corrected before the long-history inference; it was not a provider/recovery defect. The full-file assertion allows the existing ranged-read header without weakening preservation of the file contents.

The discovered error-message shape is now conservatively parsed by the opted-in window cache: the window in `context (262144)`, not the requested prompt token count. A unit regression pins that exact Strata message. Generic/unrecognized messages still infer no window.

Post-change Rust verification: `cargo test --workspace -- --skip tools::remote_tests` (isolated config): **417 passed, zero failures, 20 remote tests deliberately filtered**. Final log: `/tmp/pengyr-strata-final-tests.log`.

## Artifacts and scope

- `/tmp/pengyr-strata-live-20261023-fixed/report.json`
- `metadata.json`, `requests.json`, `history/` and `large-tool/` events and complete synthetic transcripts
- `unreduced-tool-overflow.json`, `overflow-response.json`, `discovery/report.json`
- Operator harness: `tests/live_strata_proactive.py`, shared `examples/live_proactive_context.rs`
- Reproduce only with operator approval: `python3 tests/live_strata_proactive.py --url http://10.0.23.6:8086/v1 --output-dir /tmp/pengyr-strata-<unique-name>`

No commit, push, version bump, installation or release performed. Python/C++ parity was subsequently ported and regression-tested; those ports were not part of this Rust live run. Long-length and reasoning-only reply retries are still primarily stub-tested; this run did not induce a reasoning-only empty reply or a long generation cap. These tests validate selected synthetic cases, not lossless summarization or guaranteed fit for every protected task.
