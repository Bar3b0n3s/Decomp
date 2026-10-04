# Supervision UI

The supervision UI is how a person watches and steers the agent. Its premise is that everything the
agent does is visible and auditable: every request, thinking summary, tool call, compile, diff, file
write and dollar. Live runs and past runs use the same views, because a past run is replayed from its
event log through the same state reducer. The user can pause, stop, steer, approve or take over at any
moment. Dashboards summarize, and every number drills down to its source. The UI is a separate desktop
application, `decomp-gui` (Dear ImGui), that renders immutable snapshots of a `RunState`, which is
folded from typed events, and sends commands through a `RunController`. The CLI keeps headless parity.
This document specifies the chrome, every view, the interactions, notifications, accessibility,
persistence, CLI parity, the event-driven architecture, testing and phasing.

Status: the backbone (events, `EventBus`, `RunState`, the JSONL event log with replay, and the CLI
`--progress` view) is part of the [first slice](roadmap.md#first-working-slice). `decomp-gui` and every
view below except the later-phase ones are Phase 1. Event and field names are descriptive;
`src/events/` is authoritative once it lands.

## Principles

- **Everything is visible and auditable**: requests, thinking summaries, tool calls, compiles, diffs,
  file changes and spend. Nothing the agent does happens off the record.
- **Live and past runs use the same views.** A past run is its event log replayed through the same
  reducer, so there is nothing separate to keep consistent.
- **The user is always in control.** Pause, stop, steer, approve or take over at any moment. Commands
  take effect at well-defined safe points.
- **Every number drills down.** A count opens the filtered list behind it; a dollar amount opens the
  turns that spent it; a score opens the diff that produced it.
- **The API key is never displayed.** Only whether a key is present and valid is shown.
- **The UI never blocks the workers.** It renders snapshots, never live worker state. Large tables are
  virtualized, and expensive derivations run off the render thread.

## Technology

- A separate `decomp-gui` executable that links `decomp_lib`, like the CLI. It contains views and
  view models only, and no matching or agent logic.
- **Dear ImGui** (v1.92.9, docking branch) for dockable, rearrangeable panels. Immediate mode suits
  data that changes every frame.
- **ImPlot** v1.0 for charts: progress over time, score per attempt, spend, throughput.
- **GLFW** 3.5.1 with the **OpenGL 3** backend for windowing, input and rendering.
- **ImGuiColorTextEdit** for the C++ and assembly editors, with syntax highlighting.
- Embedded UI and monospace fonts, compiled into the binary so no font files are needed at runtime.
  Their licenses are kept next to them.
- Everything is vendored and built as static libraries by premake, like the other dependencies
  ([architecture.md](architecture.md#build-system-and-dependencies)).
- Custom widgets (the treemap, the worker Gantt chart and the diff gutters with branch arrows) are
  drawn with `ImDrawList`.

## Layout and chrome

### Top bar (always visible)

From left to right:

- **Project and target name.** Clicking opens the Dashboard.
- **Run state**: idle, running, paused or stopping, shown as a colored label with text, never color
  alone.
- **Run controls**: Start, Pause, Resume, Stop (finish the current turn) and Abort. Each button is
  enabled only when its command is valid.
- **Active workers**, shown as `N/M`.
- **Spend against the run budget**: a bar with `$used / $budget`. It turns amber at 80% and red at
  100%.
- **API health light**: green, amber or red. The tooltip shows the last error and recent latency.
- **Rate-limit gauge**: requests and tokens remaining (from the latest response headers) and, during
  backoff, a countdown.
- **Global search and command palette**: functions, symbols, addresses, strings and actions.

### Status bar (always visible)

- Percentage of code bytes matched.
- Functions matched out of the total.
- Queue length.
- ETA.
- Prompt-cache hit rate (the share of input tokens served from the cache).
- The last event as one line; clicking it opens Logs & errors at that event.

### Notification center

- Toasts appear in a corner: info toasts auto-dismiss, errors stay until dismissed.
- A history panel lists every notification with filters.
- Each notification links to its source: a function, a session turn, a log entry or a setting. The
  list of notifications is [below](#notifications).

### Default layout

- Left: Function browser.
- Center: tabs for Dashboard, Agent session and Diff viewer.
- Right: Inspector.
- Bottom: tabs for Run monitor and Logs & errors.

Every panel can be docked, undocked, tabbed or closed, and layouts can be saved by name
([Persistence](#persistence)).

## Views

Each view lists what it **shows**, what the user can **do**, and its **data sources**: the events and
`RunState` fields that feed it live, and the files read for past data.

### Dashboard

**Shows**

- Target identity:
  - path and size;
  - SHA-1, verified against `decomp.json`. A mismatch is shown in red and blocks runs, so nobody
    matches against the wrong binary;
  - format and architecture, image base and entry point;
  - the Rich header's compiler and linker builds;
  - PDB status: matching GUID and age, mismatch, absent, or unsupported format.
- Progress by code bytes and by number of functions, with `library` and `skipped` shown as separate
  segments.
- Status buckets with counts and bytes: unstarted, in progress, non-matching (with a distribution of
  best match percentages), matched, refused, gave up, skipped, library.
- Progress over time: matched bytes and functions per day and per run.
- A treemap of `.text`: one cell per function, sized by bytes and colored by status or best match
  percentage. It is grouped by section, and by unit from Phase 3. Hovering shows name, size, status
  and best percentage.
- Spend summary for this run and all time: dollars, tokens by type (input, output, cache write,
  cache read), cache-hit percentage, and dollars per match.
- Recent activity: the latest matches, give-ups, refusals and errors.

**Actions**

- Click a bucket, treemap cell or feed item to open the filtered Function browser or that function's
  Inspector.
- Export a progress report as Markdown or JSON.

**Data sources**

- Project state: `decomp.json`, `symbols.txt` and `.decomp/functions/*/attempts.jsonl`.
- Run summaries (`.decomp/runs/*/summary.json`).
- Live: `RunState.functions` and the counters, updated by `FunctionStatusChanged`, `UsageRecorded` and
  `SessionFinished`.
- The numbers are the same ones `decomp status` prints.

### Run monitor

**Shows**

- A worker table: worker, function, phase, turn `n/max`, elapsed time, best percentage, tokens and
  dollars, and the last tool call. The phase is one of: building context, waiting for model,
  streaming, running tool, compiling, diffing, backoff, waiting for approval.
- A plain-language live activity feed, for example "Worker 2: compiling attempt 4 of
  `Player::Update` with MSVC 6 /O2 - 3.2 s".
- The queue, with order, difficulty estimate and ETA. The difficulty estimate is a heuristic over
  size, basic blocks, loops, calls and unknown callees, refined in Phase 3. The ETA uses this
  project's observed session durations by size bucket.
- A worker timeline: a Gantt chart of phases per worker, to spot bottlenecks such as long compile
  queues or backoff.
- Throughput: turns per minute, compiles per minute, output tokens per second, time to first token.
- Rate-limit and retry history: response-header snapshots, each retry with its status and delay.

**Actions**

- Reorder, pin and remove queue items.
- Change concurrency and budgets live.
- Skip or requeue a function.
- Pause or resume one worker.

**Data sources**

- `WorkerPhaseChanged`, `TurnStarted`/`TurnFinished`, `StreamDelta` (tokens per second, time to first
  token), `ToolCallStarted`/`ToolCallFinished`, `CompileStarted`/`CompileFinished`, `DiffComputed`,
  `RateLimitUpdated` and `RetryScheduled`.
- `RunState.workers` and `RunState.queue`.

**Notes**

The slice has one worker and no queue. The CLI `--progress` view is that single-worker version of
this view.

### Agent session

**Shows**

- A header: function, status and outcome, model, a serving-model badge when a fallback served a turn,
  effort, and budget use.
- A per-turn timeline. Each turn contains:
  - thinking summaries, collapsed by default;
  - assistant text, which streams in live;
  - each tool call's input, where a candidate source is syntax-highlighted and shown with a diff
    against the previous attempt;
  - each tool call's result: a diff summary, or compiler errors;
  - the stop reason;
  - usage per turn (input, output, cache write, cache read, dollars), latency (time to first token
    and total) and retries.
- Status lines and supervisor guidance appear inline, exactly where they were appended. Fallback
  switch points appear as markers.
- A score-per-attempt chart.
- The source version history, with a "best" badge.
- The agent's notes.
- The outcome and reason. For a refusal, the recorded category.

**Actions**

- Send a guidance message. It is appended after the next tool results, which is safe for the
  append-only conversation. It shows as pending until then and can be retracted while pending.
- Pause or end the session.
- Take over manually ([Manual mode](#manual-mode)).
- Open any attempt in the Diff viewer.
- Compare any two attempts: a source diff and an assembly diff side by side.
- Export the transcript as recorded JSONL or as readable Markdown.

**Data sources**

- Live: `StreamDelta`, `ToolCallStarted`/`ToolCallFinished`, `CompileStarted`/`CompileFinished`,
  `DiffComputed`, `UsageRecorded`, `TurnFinished`, `Refused`, `GuidanceInjected` and
  `SessionFinished`, folded into `RunState.sessions[id].turns`.
- Past: the transcript `.decomp/runs/<run-id>/sessions/<fn>.jsonl`, loaded on demand, and the
  function's `attempts.jsonl`.

**Notes**

Thinking content is the API's summary. The raw reasoning is never available, and an empty thinking
block shows as a "thinking (no summary)" marker.

### Diff viewer

**Shows**

- An objdiff-style aligned, side-by-side view of the target and candidate assembly.
- Rows colored by kind: equal, operand, opcode, insert, delete or symbol. The differing operand is
  highlighted within its row.
- Branch arrows in both gutters, offsets, and optionally the raw bytes and relocation markers.
- Symbol tooltips: address, demangled name, kind, size, and type when known.
- A data-diff panel for strings, floats and jump tables.
- A header with the score, the exact and byte-exact flags, and counts per row kind.
- Hints and binding suggestions, each linked to its rows.
- An attempt-history slider that scrubs through every attempt for the function.

**Actions**

- Edit the source with ImGuiColorTextEdit. Recompiles are debounced and run in the background; the
  latest edit wins, and the diff is marked stale while a compile runs. Diagnostics are clickable.
- Toggles:
  - normalized or raw text;
  - show relocations;
  - fuzzy registers and stack (show register-only and stack-only differences as equal);
  - differing rows only.
- Hand an edited version back to the agent. It is appended as guidance with the source attached.
- Accept a binding suggestion. This writes the symbol with `source=user` and records it in
  provenance.
- Copy rows as text.

**Data sources**

- `DiffComputed` and `attempts.jsonl` for agent attempts.
- Manual compiles use the same compile and diff engine as the agent's tool. They run as background
  jobs through `RunController`, emit the same events (tagged as manual), and are recorded as attempts
  with `origin: user`.
- `SymbolDb` for tooltips.
- The view shows the same `Report` that `decomp diff --json` prints ([matching.md](matching.md)).

### Function browser and inspector

**Shows**

- A sortable, filterable, virtualized table. Columns: address, demangled name, size, status, best
  percentage, attempts, dollars spent, last attempt, symbol source, callers and callees, and
  complexity (blocks, loops).
- The Inspector for the selected function:
  - annotated disassembly with block and loop hints;
  - cross-references (callers, callees, data references);
  - attempt history with a score chart;
  - notes;
  - status history.

**Actions**

- Filter by status, size range, name regex, unknown callees, or refused.
- Multi-select functions and queue the selection (start a run, or add to the running one).
- Mark functions skip or library.
- Reset a status to unstarted. History is kept.
- Open a function in the Diff viewer or Agent session.
- Edit notes.

**Data sources**

- `SymbolDb` and analysis results.
- Project status and history.
- Live overlay: `FunctionStatusChanged` and `DiffComputed`.

**Notes**

Tables with 100,000 or more rows stay responsive: they use `ImGuiListClipper`, and sorting and
filtering run as background jobs over the snapshot.

### Binary explorer

**Shows**

- Sections: name, address, size, characteristics.
- Imports and exports.
- Strings with cross-references.
- A hex view with symbol overlays: functions, data, strings, floats, jump tables and relocations.
- Rich header entries: product, build and count, mapped to compiler names in Phase 2.
- PDB information: path, GUID, age and match state.

**Actions**

- Follow cross-references.
- Create or rename symbols. They are recorded with `source=user` and appear in provenance.
- Jump to the function or its diff.

**Data sources**

- `pe::Image` and `pdb::Reader`, `SymbolDb` and analysis. The slice provides callers and callees;
  Phase 2 adds the full cross-reference index, Rich-header compiler names and RTTI class names.

### Symbols and provenance

**Shows**

- Every symbol: address, kind, decorated and demangled names, size, source and status.
- Who set each symbol (PDB, export, import, user or agent), when, and in which session.
- An audit trail of agent edits, each linked to the session turn that made it. Agent edits are
  bindings recorded on a match in the slice, and `set_symbol` calls from Phase 3.

**Actions**

- Rename or edit a symbol.
- Revert agent edits, one at a time or per session.

**Data sources**

- `symbols.txt` for the current state.
- `SymbolChanged` events in the run logs for the history.

### Changes and approvals

**Shows**

- Files written by Decomp or on the agent's behalf (verified sources, `symbols.txt`, later headers),
  with their diffs and the session that caused them.
- A queue of gated actions waiting for a decision.

**Actions**

- Approve or deny a gated action.
- Revert a change.
- Set the policy for each action type: auto, ask or deny.

| Action type | Default policy |
|---|---|
| Write a verified source to `src/functions/` | auto (mechanically verified) |
| Record symbol bindings from a match | auto (can be switched to ask) |
| Rename or create a symbol through `set_symbol` (Phase 3) | ask |
| Change shared headers through `define_type` (Phase 3) | ask |

**Data sources**

- `FileWritten`, `SymbolChanged`, `ApprovalRequested` and `ApprovalDecided`.

**Notes**

A gated tool call waits for its decision, and the worker shows the phase "waiting for approval". A
denial goes back to the agent as an error result so that it can adapt. Whether a long wait should
return "pending" immediately instead is open. The slice has no approval queue; it applies the
default policies.

### Cost and usage

**Shows**

- Spend by run, by function and by day.
- Tokens by type.
- Cache-hit ratio over time.
- Turns and dollars per match.
- Success rate by effort level and by model.
- A projection for the remaining functions, from the observed cost per function by size bucket.
- Budget alerts.
- Turns served by a fallback model, and usage priced without a price-table row (flagged).

**Actions**

- Set run and function budgets and alert thresholds.

**Data sources**

- `UsageRecorded`, `TurnFinished` and `BudgetUpdated`.
- Run summaries and the price table ([agent.md](agent.md#cost-accounting)).

### Toolchains and compiles

**Shows**

- The registry: name, kind, compiler path, detected version, wrapper and environment.
- Health-check results.
- Recent compiles: the full command line, environment overrides, duration, exit code, output, and
  whether it was a cache hit.

**Actions**

- Add or edit toolchains. This writes the user-level registry.
- Run a health check.
- Re-run a compile, bypassing the cache.

**Data sources**

- `ToolchainRegistry` and health-check results.
- `CompileStarted`/`CompileFinished`.

### Logs and errors

**Shows**

- The structured log, with level and module filters and search.
- Errors grouped by kind: API errors by type and status, compile failures, tool errors and I/O
  errors, each with its retry history.

**Actions**

- Copy.
- Jump to the transcript at that point (the turn or tool call).
- Open the related function.

**Data sources**

- `LogLine`, `RetryScheduled` and any event that carries an error.

### Settings

**Shows**

- API key status: present and valid. The key itself is never shown and cannot be entered here; it
  comes from the environment.
- Model, effort, budgets, fallbacks on or off, the price table, concurrency and paths.
- Theme, font size and colorblind-safe diff palettes.
- Saved layouts and recent projects.

**Actions**

- Edit any setting.
- Choose where a setting is saved: per user, or per project. Per-project settings are written to
  `decomp.json`, so they show up in git.

**Data sources**

- `decomp.json` and the user settings file.
- Key validity comes from the most recent API response (a 401 marks the key invalid). A Check button
  sends a lightweight request that lists models and consumes no tokens.

### Later-phase views

| View | Phase | Shows |
|---|---|---|
| Units | 3 | Translation units with function counts, bytes, matched percentage and cost; the per-unit queue |
| Types and layouts | 4 | Types from the project headers with exact layouts read back from the PDB, where each field is used, and differences against the target's PDB types |
| Data matching and relink | 5 | Per-section data comparison, split objects, the relink result, and SHA-1 comparison with the first differing bytes |
| Permuter and flag search | 6 | Search runs, candidates tried, best score over time, and the winning flags or permutations |

## Interactions

### Run control

| Command | Effect | Takes effect |
|---|---|---|
| Start | Builds the queue from the selection and starts the workers (one in the slice) | Immediately |
| Pause | Each worker finishes its current turn (request and tools), then waits | Between turns |
| Resume | Workers continue | Immediately |
| Stop | Workers finish their current turn, then sessions end (`ErrorCode::cancelled`) and best attempts are kept | Between turns |
| Abort | In-flight requests and compiles are cancelled and partial turns discarded | Immediately |
| Skip function | Ends that function's session, if any, and marks it `skipped` | Between turns |
| Pause worker | Pauses one worker | Between turns |
| Set concurrency, change budgets (Phase 1) | Applied to the running run | Next scheduling decision |

A turn is never left half-recorded. A stopped or paused session can be resumed or inspected exactly
as it was.

### Steering

The Agent session view has a guidance composer. A message is queued for the session and appended to
the conversation at the next safe point, after the next set of tool results, prefixed `[supervisor]`.
Until then it shows as pending and can be retracted. Once appended, it is part of the conversation
for good (the conversation is [append-only](agent.md#the-append-only-conversation)) and appears
inline in the timeline. Guidance can also carry a source: the Diff viewer's "hand back" sends the
edited source this way.

### Approvals

Gated actions appear in Changes & approvals and as notifications. A decision applies immediately.
The policy for an action type can be changed from the same place, and changes are recorded as events
so the audit trail shows who allowed what.

### Manual mode

"Take over" stops the agent's session for that function (or pauses it if the user may hand back),
then opens the Diff viewer on the best attempt. The user edits with live recompiles. "Verify and save"
runs the same mechanical check as `submit_result` (compile, diff, require `byte_exact`), then writes
the source and updates the status. "Hand back" resumes or starts a session with the user's source
appended as guidance. Manual attempts are recorded in the history like the agent's, with
`origin: user`.

### Navigation

- Every function name, address, symbol, attempt and turn is a link.
- Context menus offer "open in" any view that can show the item.
- Back and forward history covers all navigation.
- Selection is shared: selecting a function in one view updates the Inspector and any view that
  follows the selection.

### Search and command palette

One input (Ctrl+P) searches:

- functions and symbols by decorated or demangled name (fuzzy);
- addresses (typing `0x401000` jumps there);
- strings (prefix `"`);
- actions (prefix `>`), such as "Start run on selection", "Toggle raw bytes" or "Export transcript".

Every action in the UI is reachable from the palette.

### Keyboard shortcuts

Default bindings (provisional):

| Shortcut | Action |
|---|---|
| Ctrl+P | Search and command palette |
| F5 | Start / Resume |
| F6 | Pause |
| Shift+F5 | Stop (finish current turn) |
| Ctrl+Shift+F5 | Abort |
| Ctrl+Enter | Send guidance (in the composer) |
| Alt+Left, Alt+Right | Back, forward |
| F7, Shift+F7 | Next, previous differing row (Diff viewer) |
| Ctrl+B | Toggle raw bytes (Diff viewer) |
| Ctrl+S | Verify and save (manual mode) |
| Ctrl+=, Ctrl+-, Ctrl+0 | Font size up, down, reset |
| Ctrl+1 ... Ctrl+9 | Focus a view, in View-menu order |

### Export

- Progress report (Markdown or JSON).
- Transcript (JSONL as recorded, or readable Markdown).
- Diff (text or JSON, the same as `decomp diff`).
- Cost report (CSV or JSON).
- Function list (CSV or JSON, after the current filters).

## Notifications

| Notification | Severity | Toast | Opens |
|---|---|---|---|
| Function matched | Info | Yes, batched when many arrive together | Diff viewer |
| Function gave up | Info | Yes | Agent session |
| Function refused | Warning | Yes | Agent session |
| Budget at 80% (run or function) | Warning | Yes | Cost and usage |
| Budget at 100% | Error | Sticky | Cost and usage |
| Authentication error (401 or 403) | Error | Sticky | Settings |
| Rate-limit storm (many 429s in a short window) | Warning | Yes | Run monitor |
| Toolchain failure (compiler missing, health check failed, compile crash or timeout) | Error | Sticky | Toolchains and compiles |
| Run finished, with a summary (matched, gave up, refused, spend, duration) | Info | Yes | Dashboard |
| Approval requested (Phase 1) | Warning | Yes | Changes and approvals |
| A fallback model served a turn | Info | No, history only | Agent session |

## Accessibility

- **Colorblind-safe palettes.** The default diff palette is chosen to stay distinguishable under the
  common color-vision deficiencies. Alternative palettes and a high-contrast theme are in Settings.
- **Never color alone.** Each row kind also has a gutter glyph: `=` equal, `~` operand, `@` symbol,
  `!` opcode, `+` insert, `-` delete. The differing operand is boxed as well as colored, and run
  states and health lights carry text.
- **Font scaling.** Fonts scale at runtime (Ctrl+= and Ctrl+-) and follow per-monitor DPI. The
  embedded fonts are rasterized at the sizes in use.
- **Keyboard navigation.** ImGui keyboard navigation is on, every action is reachable through the
  palette, and nothing is available only on hover: tooltips are mirrored in the Inspector or a details
  pane.
- **Reduced motion.** There are no animations. Following live output (auto-scroll) can be turned off
  per view.
- **Screen readers.** Dear ImGui exposes no accessibility tree, so screen readers cannot read the GUI.
  The CLI (`decomp status`, `--progress` in plain-line mode, `--json` and the text transcripts) is the
  accessible path, with full parity for monitoring.

## Persistence

| What | Where |
|---|---|
| Window and dock layout, named saved layouts, recent projects, theme, font size, palette | User config directory (`%APPDATA%\decomp\` or `~/.config/decomp/`) |
| Per-project view state (open views, filters, column layout, sort order) | User config directory, keyed by project path |
| Agent settings, budgets and approval policies chosen per project | `decomp.json` (shared through git) |
| Runs, transcripts and history | Already on disk under `.decomp/`. The UI only reads them. |
| Anything about the API key | Nowhere |

## CLI parity

The CLI covers starting, monitoring and stopping work, and every command has `--json` output.
Steering, approvals and manual mode are GUI features.

| GUI | CLI |
|---|---|
| Dashboard | `decomp status` |
| Run monitor | `decomp agent <func> --progress`: an ANSI view redrawn in place (phase, turn, best score, spend, last tool call, tail of the streamed text); plain lines when the output is not a TTY |
| Agent session | Transcripts in `.decomp/runs/<run-id>/sessions/<fn>.jsonl` |
| Diff viewer | `decomp diff <func> --source <file>` or `--obj <file>` |
| Function browser | `decomp funcs` |
| Binary explorer | `decomp info`, `decomp disasm <func>` |
| Toolchains and compiles | `decomp toolchain list`, `decomp toolchain test` |

## Architecture

```
 session workers (AgentLoop, tools, compiles)
        | publish(Event)
        v
    EventBus --- dispatcher thread ---+--> JSONL writer --> .decomp/runs/<run-id>/events.jsonl
                                      +--> CLI progress renderer
                                      +--> RunState reducer --> snapshot: shared_ptr<const RunState>
                                                                     | atomic load, once per frame
                                                                     v
                                                              decomp-gui views
                                                                     | commands
                                                                     v
                                                              RunController --> session workers
```

### Events

Events are a `std::variant` of plain structs. Each one carries a header with a sequence number, a UTC
time, the run ID, and the session and worker IDs where they apply.

| Event | Main fields |
|---|---|
| `RunStarted`, `RunFinished` | Selection; model, effort and budgets; outcome counts and totals |
| `WorkerPhaseChanged` | Worker, phase, function |
| `SessionStarted`, `SessionFinished` | Function; outcome and reason |
| `TurnStarted`, `TurnFinished` | Turn number; stop reason, serving model, usage, cost, time to first token, latency |
| `StreamDelta` | Block index, kind (text, thinking, tool input), text |
| `ToolCallStarted`, `ToolCallFinished` | Tool and input summary; result summary, `is_error`, duration |
| `CompileStarted`, `CompileFinished` | Toolchain and command line; exit code, duration, cached flag, diagnostic count |
| `DiffComputed` | Attempt, match percentage, `exact`, `byte_exact`, row counts, hint kinds |
| `UsageRecorded`, `BudgetUpdated` | Tokens by type and dollars; budget used against limit |
| `RateLimitUpdated`, `RetryScheduled` | Header snapshot; status, delay, retry number |
| `Refused` | Refusal category, fallback information |
| `FunctionStatusChanged` | Function, old and new status, best percentage |
| `SymbolChanged` | Address, old and new name, kind, size, source |
| `FileWritten` | Path, kind, size, SHA-1, approval state |
| `GuidanceInjected` | Session, text |
| `ApprovalRequested`, `ApprovalDecided` | Action and decision (Phase 1) |
| `LogLine` | Level, module, message |

### RunState

`RunState` is a pure fold over events: `apply(RunState&, const Event&)` contains no I/O and does not
read the clock. Roughly:

```
RunState
  run         id, config, state (idle | running | paused | stopping | finished), times, budgets
  workers[]   id, phase, phase_since, session
  sessions{}  function, turns[], attempts[], best, usage, cost, outcome, pending guidance
    turns[]   n, blocks[] (thinking summary | text | tool_use | fallback), tool_calls[],
              stop_reason, serving model, usage, cost, ttft, latency
  functions{} status, best %, attempts, cost (overlaid on the project's stored state)
  counters    tokens by type, dollars, turns, compiles, compile cache hits, retries, errors
  rate_limit  requests and tokens remaining, reset times, backoff until
  queue[], approvals[] (Phase 1), notifications[], log tail (ring buffer)
```

The plain-language activity feed, the throughput figures and the chart series are derived from
`RunState` by view-model functions, which are pure and testable without ImGui.

### Snapshots

The reducer runs on the dispatcher thread. After a batch of events it publishes a new immutable
snapshot with an atomic swap, at most once per frame interval. Completed turns, attempts and log
chunks are immutable and shared between snapshots, so publishing one does not copy the history. The
exact structure-sharing scheme is open. The GUI loads the latest snapshot at the start of each frame
and never touches worker state. Streaming deltas arriving faster than the frame rate are coalesced.

### RunController

`RunController` is the only way to change anything: start(selection, config), pause, resume, stop,
abort, skip, inject_message(session, text), approve(action, decision), and in Phase 1
set_concurrency and live budget changes. Commands are queued and acknowledged through events, so the
UI shows a command as pending until the workers act on it.

### Replay of past runs

Opening a past run reads its `events.jsonl` through the same reducer and shows the result in the same
views, read-only, with the run controls disabled. Transcripts are loaded on demand when an Agent
session is opened. "Run again" starts a new run with the same selection and configuration. Because
past and live runs share one code path, anything visible live is visible afterwards.

## Testing strategy

- **Reducer unit tests.** Synthetic event sequences fold into the expected `RunState`: sessions, turns,
  counters, statuses and notifications.
- **Log round trip.** Events are written to JSONL, replayed, and must produce an identical `RunState`.
- **View-model tests.** Pure derivations (table rows, feed sentences, chart series, ETAs) are tested
  without ImGui.
- **Headless ImGui smoke test.** An ImGui context with no window or GPU backend (font atlas built,
  display size set) renders every view for several frames against synthetic snapshots: empty, huge,
  mid-stream and error states. ImGui assertions are turned into test failures. This runs in CI on all
  platforms.
- **Performance check.** A synthetic run with 100,000 functions and a high event rate must keep frame
  build time and snapshot publication within budget in Release builds.
- **Manual acceptance.** The Phase 1 exit scenario, on Windows and Linux.

## Phasing

| Part | Phase |
|---|---|
| Event types, `EventBus`, `RunState` reducer, JSONL log and replay, CLI `--progress`, single-session `RunController` | First slice |
| `decomp-gui`: chrome and every view above except the later-phase ones; multi-worker `RunController` (queue, concurrency, shared rate limiter, approvals queue, live budget changes, resumable runs) | Phase 1 |
| Binary explorer enrichment (cross-reference index, Rich-header compiler names, RTTI class names) | Phase 2 |
| Units view; approvals for `set_symbol` and `define_type` | Phase 3 |
| Types and layouts view | Phase 4 |
| Data matching and relink view | Phase 5 |
| Permuter and flag-search view | Phase 6 |

Phase 1 is done when, on Windows and Linux, a user can open a project, run 20 or more functions on 4
workers, watch live sessions, steer one, pause and stop, review diffs and changes, and reopen the
finished run from its log ([roadmap.md](roadmap.md#phase-1-supervision-gui-and-batch-runner)).
