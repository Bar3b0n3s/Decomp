# The built-in agent

Decomp's agent is a loop that Decomp itself owns. It calls the Claude Messages API directly over
HTTPS, holds **one conversation per function**, and gives the model a small set of tools: compile a
candidate and diff it, read the binary, look up symbols, keep notes, and submit a result. Decomp runs
every tool locally, re-verifies every claimed match mechanically, enforces budgets on turns, tokens,
dollars and time, retries transient API failures, and records everything (thinking summaries, tool
calls, compiles, diffs, usage and cost) as events and transcripts. This document specifies the
request defaults, prompt caching, streaming, refusal handling, the append-only conversation rules, the
loop, the tool schemas, the prompts, transcripts, cost accounting, safety and configuration, and how to
run the agent live or from offline replays.

Status: implemented in step 11 of the [first slice](roadmap.md#first-working-slice) and extended for
Phase 1, and tested offline with replays. `decomp agent` runs one function (a run with one worker);
`decomp run` and `decomp-gui` run many sessions on several workers ([Batch runs](#batch-runs)). API details
reflect the Claude API as of October 2026; check them against the current API documentation before
changing defaults.

## Components

| Component | Header | Role |
|---|---|---|
| `HttpTransport` | `agent/http.hpp` | Sends one HTTP request and streams the response body. Implementations: libcurl (Linux, macOS), WinHTTP (Windows) and `ReplayTransport` (tests, `--replay`). |
| `SseParser` | `agent/sse.hpp` | Incremental server-sent-events parser |
| `Client` | `agent/client.hpp` | Builds requests, applies retries and backoff, assembles messages from the stream (`MessageAccumulator` in `agent/messages.hpp`), captures response headers |
| `Conversation` | `agent/conversation.hpp` | The append-only history. It serializes the frozen prefix once and produces each request body. |
| `ToolRegistry` | `agent/tools.hpp` | Tool definitions, the input validator, and dispatch to handlers |
| `MatchSession` | `agent/match_session.hpp` | Per-function state: target function, toolchain setup, attempts, best attempt and source. Implements the six tools and builds the brief and the status line. The frozen system prompt lives next to it. |
| `run_loop` | `agent/loop.hpp` | The tool-use loop. Returns a `LoopOutcome` whose status is `finished`, `end_turn_without_finish`, `refused`, `budget_exhausted`, `run_budget_exhausted`, `max_turns`, `aborted`, `stopped` or `error`. |
| `LoopControl` | `agent/loop.hpp` | Thread-safe commands for a running loop: pause, resume, stop and abort (with a reason), guidance that can be retracted until it is sent, and live limits |
| `run_function` | `agent/runner.hpp` | Runs one session: wires the session, tools, conversation and loop together, publishes events, writes the transcript and updates the project |
| Price table | `agent/cost.hpp` | Usage-to-dollars accounting (`price_for`, `response_cost`), and `SpendLedger`, a run's shared spend against its budget |
| `RateGate` | `agent/rate_gate.hpp` | One per run: holds requests back while the API's rate limits are spent, and backs every session off after a 429 or 529 ([Rate limits](#rate-limits)) |
| `ApprovalGate` | `agent/approvals.hpp` | Per-action approval policies and the queue of decisions waiting for the supervisor ([Approvals](#approvals)) |

The `RunController` (`run/controller.hpp`) runs many sessions with a work queue and several workers
([Batch runs](#batch-runs)).

## Transport

Every request is `POST https://api.anthropic.com/v1/messages` (the base URL can be changed with the
`ANTHROPIC_BASE_URL` environment variable), with these headers:

| Header | Value |
|---|---|
| `x-api-key` | Read from `ANTHROPIC_API_KEY`; held in memory only |
| `anthropic-version` | `2023-06-01` |
| `content-type` | `application/json` |
| `accept` | `text/event-stream` (requests are streamed) |
| `user-agent` | `decomp/<version>` |
| `anthropic-beta` | `server-side-fallback-2026-07-01` (while fallbacks are enabled) |

The transport streams the body to the SSE parser as it arrives and exposes the response headers.
The client keeps the `request-id`, `retry-after` and `anthropic-ratelimit-*` headers of each response;
the transcript records the request ID, and a run's rate gate reads the rate-limit headers of every
response, errors included ([Rate limits](#rate-limits)). Bodies of non-2xx responses are kept (up to
64 KiB) for the error message. `Client::list_models()` sends `GET /v1/models`; the GUI's Settings view
uses it to check the key.

- **libcurl** (Linux, macOS): one handle per request. Proxies come from the usual environment
  variables, and `CURL_CA_BUNDLE` or `SSL_CERT_FILE`, and `SSL_CERT_DIR`, override the CA store.
  Redirects are not followed. While a stream is idle, libcurl calls back about once a second, so
  Abort cancels even a silent request promptly.
- **WinHTTP** (Windows): automatic proxy discovery, falling back to the WinHTTP proxy setting; TLS 1.2
  and 1.3; redirects disabled. Requests run synchronously, so Abort takes effect when the next chunk
  arrives. The API sends `ping` events while the model works, so that happens soon.

## Request defaults

| Parameter | Default | Why |
|---|---|---|
| `model` | `claude-opus-5-5` | |
| `stream` | `true` | Turns can be long; streaming avoids HTTP timeouts and feeds the live progress view. |
| `max_tokens` | `64000` | Thinking counts toward the limit; leave room for it plus a full translation unit. |
| `thinking` | `{type: "adaptive", display: "summarized"}` | Thinking cannot be disabled on this model. Summaries (and the short progress notes between tool calls) go to the transcript for supervision. The raw reasoning is never returned. |
| `output_config.effort` | `"high"` | This model's API default is `medium`, so Decomp sets the effort explicitly. Effort is fixed for a session (see [caching](#prompt-caching)). |
| `tool_choice` | `{type: "auto"}` | Forced tool choice (`any`/`tool`) is rejected with a 400 by this model. The prompt steers the model toward tools, and the loop nudges it when a turn ends without a call. |
| `tools` | The [six tools](#tools), sorted by name, each with `strict: true` and `eager_input_streaming: true` | `strict` keeps inputs schema-valid. Eager streaming sends large inputs, such as a full translation unit, as they are generated. |
| `system` | One text block (the frozen system prompt) with `cache_control: {type: "ephemeral"}` | An explicit cache breakpoint at the end of the shared prefix |
| `cache_control` (top level) | `{type: "ephemeral"}` | Automatic caching of the growing conversation |
| `fallbacks` | `"default"` | Server-side refusal fallbacks ([below](#refusals-and-fallbacks)) |

`model`, `effort` and `fallbacks` are configurable ([Configuration](#configuration)); `max_tokens` and
the thinking display are fixed in this version.

A first request looks like this. Decomp serializes JSON with sorted keys; descriptions, schemas and
the system prompt are abbreviated here:

```json
{
  "cache_control": {"type": "ephemeral"},
  "fallbacks": "default",
  "max_tokens": 64000,
  "messages": [
    {"content": [{"text": "# Target function\nfunction: int __cdecl add(int, int)\n...", "type": "text"}], "role": "user"}
  ],
  "model": "claude-opus-5-5",
  "output_config": {"effort": "high"},
  "stream": true,
  "system": [
    {"cache_control": {"type": "ephemeral"}, "text": "You are an expert reverse engineer ...", "type": "text"}
  ],
  "thinking": {"display": "summarized", "type": "adaptive"},
  "tool_choice": {"type": "auto"},
  "tools": [
    {"description": "...", "eager_input_streaming": true, "input_schema": {"...": "..."},
     "name": "compile_and_diff", "strict": true},
    {"description": "...", "eager_input_streaming": true, "input_schema": {"...": "..."},
     "name": "disassemble", "strict": true}
  ]
}
```

Request parameters are model-specific, but Decomp sends this shape to every model. `decomp agent`
refuses Claude Haiku models up front, because they do not accept adaptive thinking or `effort` (Haiku
4.5 takes a thinking token budget instead) and would fail with a 400; a per-model capability table is
planned.

## Prompt caching

The API renders `tools`, then `system`, then `messages`, and caching is a byte-prefix match. Decomp
lays requests out so that the expensive part is shared:

- **Shared prefix (tools and system).** The explicit breakpoint on the system block caches the tools
  and the system prompt together. Both are constants compiled into Decomp, so with the same model,
  thinking and effort settings every session can read that prefix from the cache while it is alive.
- **Growing tail (the conversation).** The top-level automatic `cache_control` places a breakpoint on
  the last cacheable block and moves it forward each turn. Each request reads everything up to the
  previous turn and writes only what the last turn appended. Together these use two of the four
  breakpoint slots.
- **TTL.** The default 5-minute TTL is used, and each read refreshes it. Turns normally take well under
  5 minutes. A session paused for longer pays one cache write when it resumes. The 1-hour TTL costs
  twice the input price to write and is not used.
- **Minimum size.** On `claude-opus-5-5` the minimum cacheable prefix is 512 tokens. The tools and
  system prompt are larger.
- **Verification.** `cache_read_input_tokens` is recorded per turn, in the `turn_finished` events and
  the transcript's `response` records, and the progress view shows the run's cache-hit rate. It must
  be greater than zero from the second turn of a session, and from the first turn of a session that
  starts while an earlier session's prefix is still cached. A drop to zero means something invalidated
  the prefix.
- **What would break it, and is avoided by design:** timestamps or IDs in the system prompt;
  nondeterministic tool serialization; changing tools, system, model, thinking or effort within a
  session (an effort change invalidates the conversation cache, and on some models also the system and
  tools cache); editing earlier messages.
- **Concurrency.** A cache entry becomes readable only once the first response that writes it starts
  streaming. The batch runner therefore starts the first session alone and starts the other workers
  when that session's first response begins (its `message_start`), or after 30 seconds without one
  ([Batch runs](#batch-runs)).

## Streaming

`SseParser` follows the WHATWG event-stream rules: events may be split anywhere across network chunks
(even inside `\r\n`), lines end with `\n`, `\r\n` or `\r`, multi-line `data:` fields are joined, and
comment lines are skipped. The client handles each event as follows:

| Event | Handling |
|---|---|
| `message_start` | Message ID, serving model, initial usage (input, cache write, cache read) |
| `content_block_start` | Opens a block: `text`, `thinking`, `tool_use`, `fallback` (a model switch point), or any other type, which is kept verbatim |
| `content_block_delta` | `text_delta` appends text. `thinking_delta` appends to the thinking summary. `signature_delta` appends to the block's opaque signature, stored verbatim. `input_json_delta` appends to the tool input buffer. `citations_delta` appends a citation. Unknown deltas are ignored. |
| `content_block_stop` | Closes the block. For `tool_use`, the accumulated input is parsed **strictly** as a JSON object (empty input means `{}`); the schema check follows when the tool is dispatched. |
| `message_delta` | `stop_reason`, `stop_details`, final usage (`usage.iterations` when a fallback ran). Usage counts are cumulative and replace earlier values. |
| `message_stop` | The message is complete. A connection that drops after this event still delivered a complete message. |
| `ping` | Ignored; like any data it resets the transport's stall timer |
| `error` | The stream failed. `overloaded_error` and `api_error` are [retried](#retries-and-timeouts) after discarding the partial response; other types end the request with an error. |

Text and thinking deltas become `stream_delta` events for the live progress view. They are not
written to `events.jsonl`; the transcript's `response` record has the final content.

With `eager_input_streaming`, the API no longer buffers and validates tool input, so a tool input can
arrive truncated (at `max_tokens`) or as invalid JSON. Decomp never runs a tool on input that fails the
strict parse or the schema check. The model instead receives an error result that carries the raw text,
built with the JSON library so quotes are escaped (a schema violation adds an `"error"` member naming
the offending field):

```json
{"content": "{\"INVALID_JSON\":\"<the input as received>\"}", "is_error": true,
 "tool_use_id": "toolu_...", "type": "tool_result"}
```

In the echoed assistant message, such a `tool_use` block carries `{}` as its input.

## Refusals and fallbacks

**Why fallbacks are on by default.** Claude models run safety classifiers on requests. Decomp's
prompts are about reverse engineering compiled code: disassembly, binary layouts and compiler
internals. That is legitimate work, but it sits close to security topics, and a classifier can
occasionally decline a benign request. Without fallbacks, such a false positive ends the session. With
server-side fallbacks, the API re-runs a declined request inside the same call, on another model that
Anthropic recommends for that refusal category. One false positive then costs neither the function nor
the run.

**How it is requested.** Each request carries `"fallbacks": "default"` and the beta header
`server-side-fallback-2026-07-01`. The `"default"` form lets the API choose the fallback model by
refusal category, so Decomp maintains no model list.

**What it covers.** Only policy declines trigger a fallback. Rate limits, overload and server errors
are returned as usual and go through Decomp's retry logic. Some refusal categories are not retried on
a fallback model. One of them, `reasoning_extraction`, applies to requests that ask a model to write
out its internal reasoning. Decomp never asks for that; supervision reads the summarized thinking
blocks instead.

**Reading the response.**

- A `fallback` content block (`{"type": "fallback", "from": {...}, "to": {...}}`) marks each switch
  point.
- `usage.iterations` lists every attempt; the attempt that served the response has type
  `fallback_message`.
- The top-level `model` names the model that produced the message.

Decomp records the serving model and whether a fallback block appeared (`had_fallback`) in each
`response` record of the transcript, and prices each attempt at its own model's rates
([Cost](#cost-accounting)). A serving-model badge in the Agent session view is Phase 1.

**Thinking across models.** A fallback model cannot read Claude Opus 5.5's thinking blocks. The API
drops them before that model sees them, and they are not billed. Decomp still sends every block back
unchanged and never strips blocks itself.

**Sticky routing.** After a fallback, later requests in the same conversation may be served directly
by the fallback model for about an hour. The requested model can also come back at any time. Decomp
treats the serving model as a per-turn fact, never as session state.

**Echoing a mid-output fallback.** If a model declines after producing partial output, the response
contains the partial content, a `fallback` block, and the fallback model's continuation. Before that
assistant message is sent back, Decomp omits the `thinking`, `redacted_thinking` and `tool_use` blocks
(and server-tool blocks without their pair, and any other block type it does not recognize) that
appear before the last `fallback` block, so their tool calls never run. Text blocks, paired
server-tool blocks and everything after the boundary are kept. The `fallback` blocks themselves, which
the API treats as ignorable audit markers, are not echoed. This happens once, when the turn is
committed to the `Conversation`, so every later request repeats the same bytes. The transcript keeps
the raw response.

**When the whole chain refuses.** If the final response still has `stop_reason: "refusal"`, the
session ends with outcome `refused`, the function is marked `refused`, and its history and best
attempt are kept. No tool calls from that response run, and nothing from it is appended to the
conversation. The `stop_details` category and explanation are recorded (in the outcome detail and a
`refusal` event) for information only; Decomp branches on `stop_reason`. **Decomp does not rephrase,
retry or otherwise work around a refusal.** `decomp agent` exits with code 3, and the batch runner's
default selection leaves refused functions out of later runs.

**Turning fallbacks off.** Set `agent.fallbacks` to `false` in `decomp.json`, or pass
`--no-fallbacks`. Decomp then omits the parameter and the beta header, and any decline ends the
session as `refused`.

## The append-only conversation

Rules enforced by `Conversation` and the loop:

1. `system`, `tools`, `model` and the thinking and effort settings are serialized once per session and
   reused byte-identically by every request. They are built from constants, so they are identical for
   every session with the same settings. Tools are sorted by name.
2. Assistant messages are stored as received: thinking blocks with their signatures (including blocks
   whose thinking text is empty), text, and `tool_use` blocks. The exceptions are the fallback echo
   rule above and the `{}` input of a `tool_use` block whose input was not valid JSON, both applied
   once at commit time.
3. Nothing earlier is ever edited, reordered or removed. Old tool results are not trimmed, and no
   per-request text is injected into earlier turns.
4. Every user message after the brief contains, in order: the `tool_result` blocks for all `tool_use`
   blocks of the previous assistant message (in the same order), or a nudge text block when that
   message had no tool call; then one text block holding any supervisor guidance followed by the
   status line. They stay in the history for good. Two exceptions: guidance queued before the first
   request (such as `--guidance`) is sent as its own user message right after the brief, and the
   results appended when the session ends carry no status line.
5. A response that is not committed (a failed or retried stream) leaves no trace. The retry resends
   the identical request body.

**Why.** Each thinking block's signature binds it to the exact prefix that produced it: the system
prompt, the tools and every earlier message. Editing an earlier turn invalidates every later thinking
block, and the API rejects such a request with a 400 for accounts that enforce the check. The same
discipline keeps the prompt cache warm, because a cache entry is a byte prefix. A 400, like any other
non-retryable API error, ends the session with outcome `error` and the API's message.

A session's message sequence looks like this:

```
user       brief
assistant  [thinking] [text] [tool_use compile_and_diff #1]
user       [tool_result #1] [status]
assistant  [thinking] [tool_use disassemble #2] [tool_use read_memory #3]
user       [tool_result #2] [tool_result #3] [supervisor guidance + status]
assistant  [thinking] [text]                            (no tool call)
user       [nudge] [status]
assistant  [thinking] [tool_use submit_result #4]
user       [tool_result #4]                             (appended at the end; never sent)
```

**Tested:** unit tests assert that every request of a session extends the previous one byte for byte
(the serialized `system`, `tools` and every earlier message are unchanged), including a session in
which supervisor guidance is injected mid-run.

## The loop

```
run_loop(client, conversation, tools, config, control, observer) -> LoopOutcome
    loop:
        control: abort -> aborted; stop -> stopped; pause -> wait until resumed (or stopped/aborted)
        if turns == max_turns: return max_turns
        if the wall-clock budget is spent: return budget_exhausted
        if the run's budget is spent: return run_budget_exhausted
        append the pending user message: tool results or nudge, then guidance + status line
        response = client.create_message(request)          # retries happen inside
        if response failed: return aborted (after Abort) or error
        add usage and cost
        if stop_reason == "refusal": return refused         # nothing appended, no tools run
        append the echoed assistant content
        if the response has tool calls:
            run them; cut-off or invalid calls get an is_error result instead
            if a result ends the session (submit_result): return finished
        else if 2 nudges were already used: return end_turn_without_finish
        if a budget is spent: return budget_exhausted (tokens, USD, wall clock), run_budget_exhausted
                              or max_turns
        if there was no tool call: queue a nudge
```

`run_function` turns the loop's status into the session outcome: `finished` becomes `matched` or
`gave_up` (from `submit_result`), `end_turn_without_finish` becomes `no_result`, a stop for a skip
becomes `skipped` and a stop because the run's budget ran out becomes `run_budget_exhausted`, and
`refused`, `budget_exhausted`, `run_budget_exhausted`, `max_turns`, `stopped`, `aborted` and `error`
keep their names.

**Unsubmitted matches.** When a session ends any other way than `matched` or Abort (out of turns or
budget, stopped, or without a result) and one of its attempts was byte-exact, `run_function` submits
that source itself, exactly as `submit_result` would (verification, approval, write), and the outcome
becomes `matched`. The transcript records an `auto_submit` record, and the detail says why.

**Stop reasons.** Only `refusal` is special. Any other stop reason (`tool_use`, `end_turn`,
`max_tokens`, `pause_turn`) is handled by whether the response contains tool calls. A tool call that
was still being written when `max_tokens` hit is answered with an `is_error` result asking for a
smaller call ("Your output was cut off at max_tokens while you were writing this tool call, so it was
not executed. Send it again as a smaller call ..."). A `max_tokens` response without any tool call gets
the nudge "Your previous response was cut off at max_tokens. Continue with shorter steps, and call
`submit_result` when you are done."

**Tool execution.** Each input is parsed strictly and validated. Calls run in the order the model
issued them, and every call of the turn runs, even after a `submit_result` that ends the session.
Consecutive read-only calls (`disassemble`, `read_memory`, `lookup_symbol`) run concurrently.
`compile_and_diff`, `record_note` and `submit_result` run one at a time, because they share the
session's attempt counter, best source and notes. Results always go back in **one** user message, in
the order of the `tool_use` blocks, whatever order they completed in.

**Verification of `submit_result`.** For `outcome: "matched"`, Decomp compiles the submitted `source`
again, diffs it (this counts as an attempt) and requires `byte_exact`. On success it writes the source
(into the function's unit source when it has one, see [Translation units](#translation-units), else
to `src/functions/<fn>.cpp`), and the session ends without another request; the function becomes
`matched` when the runner updates the project. On failure the model gets an `is_error` result
containing the diff, and the loop continues. `give_up` ends the session; its `reason` (which may be
empty) becomes the outcome detail.

**Outcomes and function status.**

| Outcome | Function status afterwards | `decomp agent` exit code |
|---|---|---|
| `matched` | `matched` | 0 |
| `gave_up` | `gave_up` | 2 |
| `refused` | `refused` | 3 |
| `budget_exhausted`, `run_budget_exhausted`, `max_turns`, `no_result`, `skipped`, `stopped` | `nonmatching` if any attempt of this or an earlier session scored above 0%, otherwise the previous status (`unstarted` instead of `in_progress`) | 2 |
| `aborted`, `error` | As above | 1 |

A function that was `matched` before stays `matched`. In every case the attempts, the best source, the
notes and the transcript are kept, and `symbols.txt` gets the function's new best score, attempt count
and spend ([project-format.md](project-format.md#function-status)).

### Budgets

A *turn* is one request and its response, including any retries needed to get it. Budgets apply per
function (session):

| Budget | Measured as | Default |
|---|---|---|
| Turns | Requests sent in the session | 40 |
| USD | Cost of all responses, from usage and the [price table](#cost-accounting) | 5.00 |
| Tokens | Sum of input, output, cache-write and cache-read tokens over all turns | unlimited (`0`) |
| Wall clock | Time since the session started | 30 minutes |

The turn limit and the wall clock are checked before every request; tokens, USD, wall clock and turns
are checked again after every response, once its tool calls have run. A turn already in flight is
never cut short for budget reasons; only Abort does that. When a budget runs out, the session ends
with `budget_exhausted` (`max_turns` for the turn limit). The status line tells the model how many
turns remain, how many attempts it made and its best score.

The limits are live: `LoopControl::set_limits()` (the run controller's `set_limits`, the GUI's Run
monitor, `decomp run --interactive`) changes them for running sessions, which apply them at their next
check, and for sessions that start later. The status line always reports the current limits.

A batch run can also have a **run budget** in USD (`--run-budget-usd`, `agent.max_usd_per_run`): the
sessions of the run share a `SpendLedger`, each response adds its cost, and once the total reaches
the budget no new session starts and running sessions end after their current turn with
`run_budget_exhausted`. Raising the budget during the run, or resuming the run with a higher one,
continues it.

### Retries and timeouts

| Condition | Retried? |
|---|---|
| HTTP 408, 409, 429 and 500-599 (including 529) | Yes, unless the response has `x-should-retry: false` |
| Any other HTTP error with `x-should-retry: true` | Yes |
| Network failures (DNS, connect, TLS, reset), a stalled transfer, and a stream that ends before `message_stop` | Yes |
| An `error` event in an open stream of type `overloaded_error` or `api_error` | Yes, after discarding the partial response |
| 400, 401, 403, 404, 413 and other 4xx | No. The session ends with `error` and the API's error type and message. |
| HTTP 200 with `stop_reason: "refusal"` | No. This is a content outcome ([above](#refusals-and-fallbacks)). |

Backoff is exponential with jitter: 1 s doubled per retry, capped at 60 s, then multiplied by a random
factor between 0.5 and 1, with at most 4 retries. A `retry-after-ms` header, or a `retry-after` header
in seconds (HTTP dates are not supported), sets a minimum for the delay, itself capped at 15 minutes.
Waits end early on Abort. Every retry emits a `retry` event and a `retry` record in the transcript.
The connect timeout is 30 s. The stall timeout is 120 s: a transfer fails when no byte moves for that
long, and the API sends periodic pings, so silence means a dead connection. There is no limit on total
request time, because long thinking turns are normal at high effort. Backoff waits go through an
injectable sleep function, so tests never sleep.

### Translation units

When the function's translation unit has a source ([project-format.md](project-format.md#unit-sources)),
the session works in it:

- **Which unit.** The function's unit (its `obj=`), when that is a code unit with a source path and
  the project trusts it: the unit comes from a PDB or a map, the user wrote it, or its source file
  exists already. A unit that the analysis only guessed gets a source when `decomp units emit` (or the
  user) starts it. Until then, its functions' matches go to their own files under `src/functions/`.
- **The brief** gains a `# Translation unit` section. It names the unit, its source and the functions
  that source holds, explains how candidates are composed, and shows the unit's prelude.
- **`compile_and_diff`** composes the candidate into the unit source as it is on disk. The definition
  joins the unit's functions in address order, with the `#pragma` and `#line` lines around it, and the
  candidate's other items join the prelude unless the prelude has them already. Decomp then compiles
  the whole unit under its file name (`basic.cpp`, so a `.c` unit compiles as C). Diagnostics point at
  lines of the composed unit, so each one is followed by that line's text. A candidate without a
  top-level definition of the function cannot join, and gets `unit: your source cannot join
  src/basic.cpp: ...` without a compile. When other functions of the unit are not byte-exact with the
  candidate in, the result names them: `unit: with your source in src/basic.cpp, 1 other function(s)
  there are not byte-exact: add (match 62.5% ...)`.
- **`submit_result`** requires the function to be byte-exact in the unit. Every function of the unit
  that was byte-exact before (the unit source compiled without the candidate) must stay byte-exact;
  otherwise the result is "not accepted: byte-exact itself, but with your source in src/basic.cpp
  these functions of the unit are no longer byte-exact: add (...)".
- **The approval** shows the whole unit source. The path is the unit's source, the content is the
  unit with the function composed in, the previous content is the unit before the change, and the
  summary reads "byte-exact read_counter joins src/basic.cpp (2 functions in it)".
- **The write** replaces the unit source only if it still holds what the candidate was composed into.
  If another session wrote it in between, the candidate is composed into the new content and verified
  again (up to three times). A match that no longer holds is rejected: "not accepted: src/basic.cpp
  changed while your match was being saved ...". The change in `.decomp/changes.jsonl` names the unit
  and the function.
- **History stays per function.** Attempts, `best.cpp` and notes keep the model's own source, not the
  composed unit. The runner reports the unit source as the matched source.

The GUI's Verify and save (manual mode) compiles and saves by the same rules
(`project::compile_candidate()` and `project::save_verified_function()`).

## Tools

Conventions shared by all tools:

- The definitions are part of the frozen prefix. They are declared in the first request of every
  session and never change within it, because adding a tool later would change the prefix.
- Every schema is a JSON object schema with `additionalProperties: false`, and every property is
  listed in `required` (strict tool use has no optional fields). Every tool sets `strict: true` and
  `eager_input_streaming: true`.
- Decomp's validator checks each input against the schema before the tool runs: types, required
  properties, unexpected properties and enums. The schemas declare no numeric ranges or string
  lengths; the tools apply their own limits, listed per tool. Input that fails the strict parse or the
  schema check gets an `INVALID_JSON` error result ([Streaming](#streaming)), and the tool does not
  run.
- Results are plain text. `is_error: true` marks a call that produced nothing usable: invalid input,
  an empty argument, an unknown function or address, or a rejected `submit_result`. A failed compile
  is a normal result, not an error.
- Large results are capped: listings at 400 lines, diffs at 80 rows, memory reads at 4096 bytes,
  symbol searches at 25 matches, raw compiler output at 4000 bytes.
- The definitions below are shown exactly as they are sent: keys sorted, like all of Decomp's JSON.
  Tools are sent sorted by name.

### `compile_and_diff`

Compiles a complete translation unit with the target's toolchain, flags and include directories,
extracts the function being matched, and diffs it against the target ([matching.md](matching.md)). In a
[translation unit](#translation-units), the candidate is first composed into the unit's source, and
the whole unit is compiled.
Every call is an attempt: it is recorded in `attempts.jsonl`, and an attempt that scores at least the
best so far becomes `best.cpp`.

```json
{
  "description": "Compile a complete candidate translation unit with the target's original toolchain and flags, and diff the target function against the result. Returns compiler errors, or the match percentage, the differing instructions side by side (target | candidate) and hints. Call this for every candidate you want checked.",
  "eager_input_streaming": true,
  "input_schema": {
    "additionalProperties": false,
    "properties": {
      "source": {
        "description": "Complete C/C++ translation unit defining the target function",
        "type": "string"
      }
    },
    "required": [
      "source"
    ],
    "type": "object"
  },
  "name": "compile_and_diff",
  "strict": true
}
```

An empty `source` is an `is_error` result. Otherwise the result starts with `compile: ok` (plus
`(cached)` for a [cache](matching.md#compile-cache) hit) or `compile: FAILED`, followed by the
compiler diagnostics (up to 20), the reason no diff was produced, or the compact text diff (2 rows of
context, at most 80 rows; [matching.md](matching.md#8-output-formats)), and ends with the attempt
number and the best score so far. Three results from scripted sessions with clang-cl (in the last
two, an earlier session had already matched the function, hence the best score):

```
compile: ok
match 68.8% (2/4 equal; 1 operand, 1 opcode) - not matching
target ?add@@YAHHH@Z (15 bytes) | candidate ?add@@YAHHH@Z (15 bytes)
~    0: mov eax, dword ptr [esp+0x8]                  |    0: mov eax, dword ptr [esp+0x4]  (op1 stack)
!    4: add eax, dword ptr [esp+0x4]                  |    4: sub eax, dword ptr [esp+0x8]
     8: add eax, dword ptr [g_counter]                |    8: add eax, dword ptr [g_counter]
     e: ret                                           |    e: ret

attempt 1: best so far 68.8%
```

```
compile: FAILED
line 1:40: error: use of undeclared identifier 'nope'

attempt 1: best so far 100.0%
```

```
compile: ok
diff: the candidate object does not define '?add@@YAHHH@Z' (it defines: ?plus@@YAHHH@Z)
attempt 2: best so far 100.0%
```

None of these is an `is_error` result. The candidate's function is found by its decorated name or by
an equivalent name ([matching.md](matching.md#2-candidate-side)); in the last result the candidate
defined a function with a different name.

### `disassemble`

```json
{
  "description": "Annotated disassembly of any function in the target program (callers, callees, helpers). Use it to learn signatures, calling conventions and structure layouts.",
  "eager_input_streaming": true,
  "input_schema": {
    "additionalProperties": false,
    "properties": {
      "target": {
        "description": "Function name or address (e.g. \"add\", \"?add@@YAHHH@Z\", \"0x401060\")",
        "type": "string"
      }
    },
    "required": [
      "target"
    ],
    "type": "object"
  },
  "name": "disassemble",
  "strict": true
}
```

`target` is a decorated, readable or PDB name, or an address (`0x401060`, `401060h`). The result is
the annotated listing that `decomp disasm --no-bytes` prints, cut to 400 lines with a note on how
many were omitted. An unknown target is an `is_error` result: "unknown function 'nope'; try
lookup_symbol".

### `read_memory`

```json
{
  "description": "Read data from the target image: string contents, tables, constants, initial values of globals. Use it when the disassembly references data you need to reproduce exactly.",
  "eager_input_streaming": true,
  "input_schema": {
    "additionalProperties": false,
    "properties": {
      "address": {
        "description": "Address or symbol name, optionally +offset (e.g. \"0x402010\", \"g_table+0x8\")",
        "type": "string"
      },
      "count": {
        "description": "Number of elements to read (bytes for \"bytes\"; ignored for \"string\")",
        "type": "integer"
      },
      "format": {
        "description": "How to interpret the memory",
        "enum": [
          "bytes",
          "string",
          "u8",
          "u16",
          "u32",
          "u64",
          "i8",
          "i16",
          "i32",
          "i64",
          "f32",
          "f64",
          "pointer"
        ],
        "type": "string"
      }
    },
    "required": [
      "address",
      "count",
      "format"
    ],
    "type": "object"
  },
  "name": "read_memory",
  "strict": true
}
```

`count` is clamped to 1-4096, and at most 4096 bytes are read. `bytes` prints a hex dump, 16 bytes per
line; `string` reads a NUL-terminated string of up to 4096 characters and prints it escaped; the
integer and float formats print one indexed value per line; `pointer` prints pointer-sized values with
the symbol each one points to. The result starts with the address's symbol and value, for example
`int *g_table+0x8 (0x40300c):`. An address outside the image, or an unknown symbol, is an `is_error`
result. Bytes that are not backed by the file (such as `.bss`) read as zero, with the note
"(uninitialized data reads as zero)".

### `lookup_symbol`

```json
{
  "description": "Find symbols by exact name, partial name or address; returns addresses, kinds, sizes and readable signatures. Use it to get the exact declaration of something the function references.",
  "eager_input_streaming": true,
  "input_schema": {
    "additionalProperties": false,
    "properties": {
      "query": {
        "description": "Name, part of a name, or address",
        "type": "string"
      }
    },
    "required": [
      "query"
    ],
    "type": "object"
  },
  "name": "lookup_symbol",
  "strict": true
}
```

A query that resolves to an address (an address, or an exact name) returns the symbol at or around
that address. Otherwise the query is matched, ignoring case, as a substring of the decorated, readable
and PDB names, and up to 25 matches are listed (then "... more matches; refine the query"). Each match
is one line: address, kind, size, readable name and decorated name, for example
`0x403000 data     size 4     int g_counter  [?g_counter@@3HA]`. An empty query is an `is_error`
result.

### `record_note`

```json
{
  "description": "Save a short note for future attempts on this function (what you learned, what did not work). Notes persist across sessions.",
  "eager_input_streaming": true,
  "input_schema": {
    "additionalProperties": false,
    "properties": {
      "text": {
        "description": "Short note for future attempts on this function",
        "type": "string"
      }
    },
    "required": [
      "text"
    ],
    "type": "object"
  },
  "name": "record_note",
  "strict": true
}
```

An empty `text` is an `is_error` result. Notes are appended to `.decomp/functions/<fn>/notes.md` as
`- YYYY-MM-DD HH:MM: <text>` and shown in the briefs of later sessions; the result is "noted". Without
a project, notes are not saved.

### `submit_result`

```json
{
  "description": "Finish this function. Use outcome \"matched\" with the exact source once compile_and_diff reported a byte-exact match (it is re-verified), or \"give_up\" with your best source and the reason when stuck.",
  "eager_input_streaming": true,
  "input_schema": {
    "additionalProperties": false,
    "properties": {
      "outcome": {
        "description": "matched (re-verified) or give_up",
        "enum": [
          "matched",
          "give_up"
        ],
        "type": "string"
      },
      "reason": {
        "description": "Why you are giving up (empty when matched)",
        "type": "string"
      },
      "source": {
        "description": "The matching source, or your best attempt when giving up",
        "type": "string"
      }
    },
    "required": [
      "outcome",
      "reason",
      "source"
    ],
    "type": "object"
  },
  "name": "submit_result",
  "strict": true
}
```

For `matched`, `source` must not be empty; it is verified as described [above](#the-loop). An accepted
match returns "accepted: byte-exact match verified."; a rejected one returns an `is_error` result that
starts with "not accepted: the submitted source is not byte-exact." followed by the diff. `give_up`
returns "recorded: gave up (<reason>). Best match <n>%." and ends the session.

### Later tools

| Tool | Phase | Purpose |
|---|---|---|
| `set_symbol` | 3 | Name an address, or set its kind and size. Provenance `agent`; subject to the approval policy. |
| `define_type` | 3 | Add or replace a type declaration in the project's shared headers |
| `get_type` | 4 | Return a type's exact layout (sizes and offsets read back from a PDB) |
| `search_matched_examples` | Later | Find matched functions in the project with a similar shape, to reuse idioms |

New tools take effect at session boundaries, because a tool list is part of the frozen prefix.

## Prompts

**System prompt.** Compiled into the binary (`system_prompt()` in `agent/match_session.cpp`), frozen,
and identical for every session. A version number and SHA-1 for it in run summaries are planned; until
then, the first `request` record of every transcript contains the full text. It covers:

1. *Context.* The model is an expert reverse engineer on a matching-decompilation project: rewriting
   functions of a compiled x86/x64 program as C/C++ that the original compiler and flags turn into
   byte-identical code. This is preservation and interoperability work, and the user is entitled to
   study the binary.
2. *Tools and scope.* One target function per conversation, and what each of the six tools is for.
3. *Method.* Read the brief; work out the signature, calling convention and types (disassembling
   callers or callees when needed); write a first complete candidate and compile it early; fix
   structural differences before operand-level ones.
4. *Output rules.* A complete, self-contained translation unit that declares everything it uses, with
   declarations that produce the decorated names shown in the brief (with examples of MSVC name
   decoration, `extern "C"` names and arrays). No inline assembly; only the headers the brief lists.
   When the brief has a `# Translation unit` section, the source is composed into that unit's source,
   and the section's rules apply.
5. *Matching tips for MSVC and clang-cl.* Register allocation and instruction order follow declaration
   order, expression order, temporaries and scope; stack offset differences point to local variable
   order, size or type; signedness and width change instructions; an inverted branch means swapped
   if/else bodies or a negated condition; switches become jump tables; calls must use the right
   calling conventions; string literals and floating-point constants are compared by value; when only
   registers or stack offsets differ, try small reorderings.
6. *Finishing.* Keep text between tool calls short. Submit a byte-exact result with `submit_result`
   and the exact source; when stuck after many attempts, record a note and give up with the best
   source and the reason.

The prompt never asks the model to write out its internal reasoning. It does not yet tell the model
that strings and names from the binary are data, not instructions (planned).

**Per-function brief.** This is the first user message, built from the analysis results and the
function's history. The brief of the CI smoke test's scripted session:

````
# Target function
function: int __cdecl add(int, int)
symbol:   ?add@@YAHHH@Z  (PDB name: add)
address:  0x401060, 15 bytes
toolchain: clang-cl-x86 (clang_cl), flags: /O2 /Gy /GS- /GR- /EHs-c-
project headers available: (none)

# Annotated disassembly
```
; function: int __cdecl add(int, int)
; symbol:   ?add@@YAHHH@Z  (pdb: add)
; range:    0x401060-0x40106f (15 bytes, 4 instructions, 1 blocks, 0 loops)
; callers:  int __cdecl dispatch(int, int), entry
; data:     g_counter (data)
  00401060  mov eax, dword ptr [esp+0x8]                         ; arg_4
  00401064  add eax, dword ptr [esp+0x4]                         ; arg_0
  00401068  add eax, dword ptr [g_counter]
  0040106e  ret
```

# Referenced symbols
- data g_counter: int g_counter, initial bytes: 03 00 00 00

# Callers
int __cdecl dispatch(int, int), entry

# Translation unit
This function belongs to the unit basic.obj (12 functions). Its source will be src/basic.cpp (C++): no function of the unit is matched yet, so yours starts it.
Your source is composed into it: your definition of the function joins the unit's functions in address order (with the #pragma lines around it), and your other declarations, types and data join the unit's prelude unless it has them already. compile_and_diff compiles the whole unit that way, and submit_result also checks that the unit's other functions stay byte-exact. Define the function at the top level of your source, not inside a class or namespace block; do not define the prelude's types differently.

Write a candidate with the function and what the unit's prelude lacks, and call compile_and_diff.
````

The listing is cut to 400 lines. Initial bytes are shown for data symbols of up to 64 bytes. The
`# Translation unit` section appears when the session works in the function's
[unit](#translation-units). Once the unit's source holds functions, the section lists them and shows
the unit's prelude (up to 200 lines) in a code block. For a C unit, the section adds "write C, not
C++". Without a unit, the last line asks for "a complete candidate translation unit". When the project
has history for the function, an `# Earlier attempts` section follows: the number of earlier attempts
and the best score, the notes, and the best source so far.

**Status line and nudges.** These are appended as text blocks ([rules](#the-append-only-conversation))
and never removed. A status line, a nudge, and guidance as the model sees them:

```
[status] turns left: 37; attempts: 2; best match: 100.0%
You ended your turn without calling `submit_result`. Continue working on the task, and call `submit_result` when you are done.
[Supervisor guidance] Try declaring the loop counter before the pointer.
```

The status line does not mention spend; the budgets are enforced by the loop.

## Batch runs

`decomp run` and `decomp-gui` run the agent on many functions under a `RunController`
(`run/controller.hpp`), and `decomp agent` on one (one worker, no stagger, no run budget). Each
function gets one session (`run_function`: one conversation, with its tools, budgets and outcomes);
the controller adds the queue, the workers and the run-wide controls. So a `decomp agent` run has the
same run directory as a batch run: `decomp runs list` and the GUI list it, and a stopped one resumes
with `decomp run --resume`.

**Queue and workers.** The functions are queued in the order given, or for a selection by an estimate
of difficulty (from the function's size), easiest first. N workers (`--workers`, `agent.workers`,
default 4, at most 64) each take the next pending function and run one session at a time. Pinned
functions go first. Functions can be added, removed, moved, pinned, skipped (a running session stops
after its turn with `skipped`) and requeued (a finished function runs again, with a new session)
while the run goes on. Pause holds every worker (or one) before its next request; Stop lets running
sessions finish their current turn and leaves the rest of the queue pending; Abort also cancels the
requests and compiles in flight. Concurrency can be raised or lowered during the run; workers above
a lowered count retire when their session ends.

**Staggered start.** Until the first session's first response begins streaming (or 30 seconds pass),
only one session runs. The others then start with the shared prompt prefix (tools and system prompt)
already in the cache, and read it instead of each writing it ([Prompt caching](#prompt-caching)).

**Run budget and live limits.** See [Budgets](#budgets): a run budget in USD shared by all sessions,
and per-function limits that can change while sessions run.

**Guidance.** Guidance for a running session (`:guide <fn> <text>` in `decomp run --interactive`,
the GUI's composer) is queued with an id and appended to the session's next user message. Until that
message is sent, it can be retracted. The `guidance` event and transcript record carry the id.

### Rate limits

The run's sessions share one `RateGate`. It reads the `anthropic-ratelimit-*` headers of every
response, errors included. Before each request, a session waits while the requests allowance is spent,
or while less than 2% of the input or output token allowance is left, until the allowance's reset time
(at most a minute per wait, then it checks again). A 429 or 529 puts every session into backoff until
its `retry-after` time (10 s for a 429 and 2 s for a 529 without one; at most 5 minutes), on top of
the session's own retry delay. Waits end early on Abort. Each change is published as a
`rate_limit_updated` event, and sessions that wait show the phase "waiting for rate limit".

### Approvals

Some actions can need the supervisor's approval. The one gated action is `write_source`: saving a
verified match into its unit's source or to `src/functions/`. Each action has a policy:

| Policy | Effect |
|---|---|
| `auto` (default) | The source is saved at once. |
| `ask` | The match waits: `approval_requested` is published, the session's phase becomes "waiting for approval", and the supervisor approves or denies it, with an optional reason, in the GUI's Changes and approvals view. Stop does not cancel the wait; Abort does. Only the GUI can answer, so `decomp run` refuses `ask` (an `ask` in `decomp.json` must be overridden with `--policy`). |
| `deny` | Matches are never saved. |

The check comes after the byte-exact verification and before the write. A denied match is not saved,
and the model gets a tool error: "Verified byte-exact, but the supervisor declined saving it ..."
with the reason, so it can adjust the source or give up. A declined source is not submitted again
automatically at the end of the session. Policies come from `agent.approvals` in `decomp.json`
(`{"write_source": "ask"}`), `decomp run --policy write_source=deny`, or the GUI, which can change a
live run's policy. `file_written` events and `.decomp/changes.jsonl` record how each write was
approved.

### Resuming

A run's directory holds its settings, queue and event log
([project-format.md](project-format.md#runsrun-id)). `decomp run --resume <id>` (or Resume in the GUI)
continues a run that stopped, ran out of budget or was interrupted (its process died while `run.json`
still said it was running): functions that finished stay finished, and interrupted, stopped, aborted
and failed ones start again with a fresh conversation. The new session's brief carries the attempts,
notes and best source of the earlier ones, so little is lost. The run keeps the settings it recorded
(model, effort, workers, budgets, limits, policies) unless they are overridden on the command line,
its event numbering continues, and `run_resumed` lists the functions whose sessions were interrupted.
A run can be resumed only by one process at a time (`run.lock`), and a project runs one run at a time
(`.decomp/active-run.lock`, which `decomp agent` takes too).

## Transcripts and event logs

Run data lives in the project's `.decomp/` directory ([project-format.md](project-format.md)), or under
`--log-dir`:

```
.decomp/runs/<run-id>/events.jsonl              every event of the run except stream deltas, in order
.decomp/runs/<run-id>/sessions/<fn>.jsonl       the transcript of a function's first session in the run
.decomp/runs/<run-id>/sessions/<fn>.<n>.jsonl   the transcript of its n-th session (after a requeue or resume)
.decomp/runs/<run-id>/summary.json              totals: status, model, effort, cost, per-function outcome and usage
.decomp/runs/<run-id>/run.json                  batch runs: settings, status and queue ([project-format.md](project-format.md#runsrun-id))
.decomp/functions/<fn>/attempts.jsonl           every compile attempt (fed into future briefs)
```

Session ids are `<run-id>-<va>` for a function's first session in a run and `<run-id>-<va>-<n>` for
later ones, so every session has its own transcript. A session writes each transcript record before
it publishes the matching event, so a view that sees an event can read the record.

A transcript is one JSON object per line, distinguished by `type`:

| `type` | Contents |
|---|---|
| `session` | The header: `session`, `function`, `display`, `va`, `model`, `effort`, `worker` |
| `request` | The first request body in full (`turn` 1): model, settings, system prompt, tools and the brief |
| `request_delta` | For every later turn: the messages appended since the previous request. History is append-only, so these reconstruct each request body exactly. |
| `response` | Per turn: message `id`, serving `model`, `stop_reason`, `stop_details`, the raw `content` (thinking blocks with signatures, `fallback` blocks), the raw `usage` (with `iterations`), `had_fallback`, `cost_usd`, `latency_ms`, `ttft_ms` (time to the first streamed event), `request_id` |
| `tool` | Per tool call: `turn`, `id`, `name`, `input` (the raw text when it was not valid JSON), `is_error`, `result`, `elapsed_ms` |
| `retry` | A retried request: `turn`, `attempt`, `error`, `delay_ms`, HTTP `status`, `retry_after_ms` |
| `guidance` | Supervisor guidance as it was sent: `turn`, `id`, `text` |
| `paused`, `resumed` | Pause and resume points (`turn`) |
| `auto_submit` | An unsubmitted byte-exact attempt submitted at the end: `accepted`, `result` |
| `outcome` | `outcome`, `detail`, `best_match`, `turns`, `cost_usd`, `usage` |

**Never recorded:** the API key and the request headers. The scripted-replay transport also masks
`x-api-key` in the requests it records.

## Cost accounting

`response_cost` converts usage into dollars with a price table compiled into Decomp
(`agent/cost.cpp`). The prices, in USD per million tokens, are:

| Model | Input | Output | Cache write (5 min) | Cache read |
|---|---|---|---|---|
| `claude-opus-5-5` | 4.00 | 20.00 | 5.00 | 0.20 |
| `claude-opus-5` | 5.00 | 25.00 | 6.25 | 0.50 |
| `claude-opus-4-8` | 5.00 | 25.00 | 6.25 | 0.50 |
| `claude-sonnet-5-5` | 2.00 | 10.00 | 2.50 | 0.20 |
| `claude-fable-5-1` | 10.00 | 50.00 | 12.50 | 0.25 |
| `claude-haiku-4-5` | 1.00 | 5.00 | 1.25 | 0.10 |

```
usd = (input_tokens * input + output_tokens * output
       + cache_creation_input_tokens * cache_write + cache_read_input_tokens * cache_read) / 1e6
```

- When `usage.iterations` is present (a fallback ran), each attempt is priced at the rates of the model
  that ran it (an entry without a model at the configured model's rates), and the turn's usage is the
  sum of all attempts. Otherwise the top-level usage is priced at the response's `model`.
- A model ID matches its table row exactly, or through the longest row that is a prefix followed by
  `-` or `@` (dated or platform variants such as `claude-opus-5-5-20270101`).
- A serving model without a row is priced at the configured model's rates. If the configured model
  has no row either, everything is priced at the `claude-opus-5-5` row, and a warning says which
  prices the spend and the USD budget are estimated with.
- Thinking is billed as output and is included in `output_tokens`. `input_tokens` covers only the
  uncached part of the prompt; the full prompt size is input plus cache write plus cache read.
- Decomp uses the 5-minute cache TTL, so 1-hour cache writes (twice the input price) are not in the
  table.
- Prices change. The table is code; making it configurable is planned.

Example: a turn with 2,000 uncached input tokens, 30,000 cache-read tokens, 3,500 cache-write tokens
and 9,000 output tokens on `claude-opus-5-5` costs 0.008 + 0.006 + 0.0175 + 0.18 = **$0.2115**. Costs
roll up per turn (`turn_finished` events), per session (the outcome, `summary.json`), per function
(`cost=` in `symbols.txt`, which `decomp status` sums) and per run (`summary.json`).

## Safety

- **Compiled, never executed.** No tool runs target or candidate code. Decomp contains no emulator
  and never launches the target.
- **Writes are confined to the project.** The agent has no general file-writing tool. Decomp itself
  writes only to unit sources (`src/<unit>.cpp`, at the paths `units.txt` gives) and
  `src/functions/<fn>.cpp` (verified sources), `symbols.txt` (statuses, scores and spend) and
  `.decomp/` (history, runs, build directories, cache). Paths come from `units.txt` and sanitized
  function keys, never from model output.
- **Compiler inputs are checked (planned).** Each candidate compiles in a fresh directory. Rejecting
  `#include` directives with absolute paths or `..` escapes outside the configured include directories,
  as well as MSVC `#import`, is planned; until then a candidate could pull local files into
  diagnostics that go back to the API.
- **The API key stays in memory.** It is read from `ANTHROPIC_API_KEY` and never written to project
  files, transcripts, event logs or log files. `decomp agent` refuses to start without it, except with
  `--replay`, which sends nothing.
- **What leaves the machine:** the system prompt, the tool definitions, the per-function brief
  (annotated disassembly, referenced data, symbol names, notes and the best previous source) and tool
  results (diffs, diagnostics, disassembly, memory reads). Use the agent only on binaries whose code
  you are comfortable sending to the API.
- **Untrusted content.** Strings and names from the target appear in prompts. The narrow tool surface
  (read-only queries plus compile) bounds what injected text could do; marking such content as data in
  the system prompt is planned.
- **Control.** Budgets cap spend, Abort is always available (Ctrl+C twice), and refusals are respected.

## Configuration

Agent settings live in the `agent` object of `decomp.json`, so they are shared with the project, and
`decomp agent` and `decomp run` options override them for one run. The GUI's Settings view edits
them.

| Key | Default | Option | Notes |
|---|---|---|---|
| `model` | `claude-opus-5-5` | `--model` | |
| `effort` | `high` | `--effort` | `low`, `medium`, `high`, `xhigh` or `max`; fixed for a session |
| `fallbacks` | `true` | `--no-fallbacks` | `false` disables server-side fallbacks |
| `max_turns` | `40` | `--max-turns` | Requests per session |
| `max_usd_per_function` | `5.0` | `--budget-usd` | `0` means unlimited |
| `max_tokens_per_function` | `0` | `--max-tokens` | All four usage fields combined; `0` means unlimited. Cache reads grow every turn, so USD is the primary budget. |
| `max_minutes_per_function` | `30` | `--max-minutes` | Wall clock; `0` means unlimited |
| `workers` | `4` | `--workers` (`decomp run`) | Sessions that run at once |
| `max_usd_per_run` | `0` | `--run-budget-usd` (`decomp run`) | The run budget; `0` means unlimited |
| `approvals` | `{}` | `--policy action=policy` (`decomp run`) | Per-action approval policies ([Approvals](#approvals)); actions not listed are `auto` |

Fixed in this version (configurable later): `max_tokens` 64000, thinking display `summarized`, 2
nudges, 4 retries with a 1 s base and a 60 s cap, a 30 s connect timeout, a 120 s stall timeout, the
stagger timeout of 30 s, the tool limits and the price table.

The API key is the one setting that never lives in a file: it comes from `ANTHROPIC_API_KEY`.

## Running live

1. Build Decomp, create a project (`decomp init <binary> --toolchain <name> --flag ...`), and make
   sure `decomp toolchain test <name>` passes for the project's toolchain.
2. Set the key:

   ```sh
   export ANTHROPIC_API_KEY=<your key>          # Linux, macOS
   $env:ANTHROPIC_API_KEY = "<your key>"        # Windows PowerShell
   set ANTHROPIC_API_KEY=<your key>             # Windows cmd
   ```

3. Run one function:

   ```sh
   decomp agent sum_array
   ```

   The live progress view is on by default (on stderr; `--no-progress` hides it, and `--progress`
   keeps it with `--json` or `-q`). The first Ctrl+C stops after the current turn, a second aborts the
   request in flight, and a third exits at once. With `--interactive`, every line typed on stdin is
   queued as guidance for the next request, except `:pause`, `:resume`, `:stop`, `:abort` and
   `:help`. `--guidance <text>` (repeatable) sends guidance with the first request. Exit codes: 0
   matched, 2 not matched (gave up, budget, turn limit, no result, stopped), 3 refused, 1 error or
   aborted.
4. Check the results:
   - `decomp status`;
   - the source: in its unit's source (`src/basic.cpp` for the fixture), or in `src/functions/`;
   - the transcript in `.decomp/runs/<run-id>/sessions/<fn>.jsonl`, where the `response` records
     should show `cache_read_input_tokens` greater than zero from the second turn.

Without a project, `decomp agent <func> --binary <exe> --toolchain <name>` works on a bare binary;
nothing is persisted unless `--log-dir <dir>` is given, and the best source is printed at the end.

**Many functions.** `decomp run --all --workers 4 --run-budget-usd 20` runs every function the default
selection takes (or name them, or choose with `--status unstarted,nonmatching` or `--filter <regex>`).
The progress view shows one line per worker. With `--interactive`, stdin takes `:pause [worker]`,
`:resume [worker]`, `:stop`, `:abort`, `:skip <fn>`, `:requeue <fn>`, `:workers <n>`, `:budget <usd>`,
`:guide <fn> <text>` and `:status`. Exit codes: 0 completed, 2 stopped or out of budget (resumable), 1
aborted or failed. `decomp runs list` shows the project's runs, `decomp runs show <id>` summarizes
one, and `decomp run --resume <id>` continues it ([Resuming](#resuming)). In `decomp-gui`, open the
project (File > Open project) and start a run from the top bar; the Run monitor and Agent session
views steer it ([ui.md](ui.md)).

## Offline replay testing

All agent tests run without a network or a key:

- `ReplayTransport` serves scripted HTTP exchanges in order from a JSONL file (blank lines and `#`
  comments are skipped) and records the outgoing requests for assertions, with `x-api-key` and
  `authorization` masked. Each line is one response:

  ```
  {"events": [{"data": {"type": "ping"}, "event": "ping"}], "headers": {"request-id": "req_1"}, "status": 200}
  {"sse": "event: ping\ndata: {\"type\": \"ping\"}\n\n", "status": 200}
  {"body": {"error": {"message": "Overloaded", "type": "overloaded_error"}, "type": "error"}, "headers": {"retry-after": "0"}, "status": 529}
  {"network_error": "connection reset"}
  ```

  `events` lists the response's SSE events, from `message_start` to `message_stop` (shortened to one
  `ping` above), and is serialized as SSE (`"crlf": true` switches to CRLF line endings). `sse` is
  raw SSE text, `body` is a plain response body, and `"disconnect_after": N` delivers only the first
  N bytes and then fails with a network error. Successful bodies arrive in pseudo-random chunks of
  1-61 bytes, to exercise parsing across chunk boundaries. When the script runs out, the request
  fails.
- Scripted sessions:
  - `tests/replay/agent_match_add.jsonl` matches `add` in the x86 fixture with clang-cl: two lookups in
    parallel, a wrong attempt, its diff, a corrected attempt, `submit_result`, `matched`. The CI smoke
    test and the Windows MSVC round trip run it through `decomp agent`, and check with `decomp units
    verify` that the unit source it starts verifies.
  - `tests/replay/agent_session_sum_array.jsonl` drives the loop with stub tools: thinking with
    signatures, two parallel read-only calls, a 429 with `retry-after`, two compiles, a turn that ends
    without a tool call (nudged), and `submit_result`.
  - The unit tests script further sessions in code: a wrong source, its diff, a corrected source and
    `submit_result` with real clang-cl compiles (`matched`, and the source written to the project), a
    session in a unit (the brief's section, a candidate that breaks another function of the unit and
    is refused, a candidate that cannot join, the unit source written), a
    refusal (`refused`, no tools run), budget exhaustion (`budget_exhausted`), supervisor guidance
    injected mid-run (the history stays append-only), a stop between turns, pause, resume and abort, a
    mid-output fallback, a tool call cut off at `max_tokens`, invalid and schema-invalid tool input,
    429 and 529 responses, overloaded stream errors, network errors, truncated streams, and SSE edge
    cases (thinking and signature deltas, tool JSON split mid-token, CRLF and CR line endings).
- Assertions cover outcomes and function statuses, the emitted events, the append-only property, the
  written transcripts and history, and the absence of the API key: the runner test sets a sentinel key
  and checks that no transcript line contains it.
- From the CLI, in a project created with
  `decomp init tests/fixtures/x86/basic.exe --dir <dir> --toolchain clang-cl-x86 --flag /O2 --flag /Gy
  --flag /GS- --flag /GR- --flag /EHs-c-`, the command `decomp -C <dir> agent add --replay
  tests/replay/agent_match_add.jsonl` runs the same path end to end. Only the model's side is
  scripted; the compiles are real, so clang-cl must be installed.
- Batch runs take a directory of scripts, one per function: `--replay-dir <dir>` (and the GUI's
  developer setting) gives each session the first script that exists of `<safe name>.jsonl` (for
  example `Player__Hit_401000.jsonl`), the same name without the address (`Player__Hit.jsonl`) and
  `default.jsonl`. `tests/replay/run/` (generated by
  `tests/replay/make_run_scripts.py`) matches 8 of the x86 fixture's functions with their real sources
  and gives up on the rest; `decomp -C <dir> run --all --workers 4 --replay-dir tests/replay/run` runs
  all 13, and CI does so through the CLI and through `decomp-gui --run-all` under Xvfb. The run
  controller's own tests use a fake session function: 200 functions on 4 workers, pause, per-worker
  pause, stop, abort, skip, requeue, queue edits, concurrency changes, run budgets, approvals and a
  resume after a simulated crash.

## Open questions

- **Guidance channel.** Supervisor guidance could be sent as mid-conversation `role: "system"`
  messages, the API's operator channel, instead of text blocks after the tool results. Both keep the
  cache intact. The slice uses text blocks; revisit once the loop is stable.
- **Per-message effort.** The per-message effort change (a beta feature) could raise effort for hard
  turns without invalidating the cache.
- **Bindings and approval.** Should consistent new symbol bindings count toward `matched` without
  user approval ([matching.md](matching.md#3-canonicalization))?
- **Capturing replay files.** A capture mode could record replay files from live sessions. Transcripts
  already exclude credentials.
