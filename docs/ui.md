# Supervision UI

The supervision UI is how a person watches and steers the agent. Its premise is that everything the
agent does is visible and auditable: every request, thinking summary, tool call, compile, diff, file
write and dollar. Live runs and past runs use the same views, because a past run is replayed from its
event log through the same state reducer. The user can pause, stop, steer, approve or take over at any
moment. Dashboards summarize, and every number drills down to its source. The UI is a separate desktop
application, `decomp-gui` (Dear ImGui), that renders immutable snapshots of a `RunState`, which is
folded from typed events, and sends commands through a `RunController`. The CLI covers the headless
subset ([CLI parity](#cli-parity)). This document specifies the chrome, every view, the interactions,
notifications, accessibility, persistence, CLI parity, the event-driven architecture, testing and
phasing.

Status: the backbone (typed events, the serialized `EventBus`, the `RunState` reducer and its
snapshots, the JSONL event log and its replay, the CLI progress view) and the `RunController` are
implemented ([Architecture](#architecture)). `decomp-gui` has the chrome, projects, live and past runs,
and every view below but the Phase 6 one. Parts that later phases add are marked as such. Data sources name event
types ([Events](#events) lists them).

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
    matches against the wrong binary; a project that records no SHA-1 shows it as unverified;
  - format (PE32 or PE32+), architecture, EXE or DLL, and the linker version; image base and entry
    point (a link to its function);
  - what the target was built with: the compiler its own code came from, with its version and
    Visual Studio release, the toolchain name for that release (a link to Toolchains and compiles)
    and notes that matter for matching (an edition without an optimizer, LTCG, PGO, other compilers,
    assembly objects); the Rich header's compiler and linker builds (the other entries folded), and a
    warning when its checksum does not match;
  - PDB status: matching GUID and age, mismatch, absent, or unsupported format, with the PDB loaded
    and the debug record (path, GUID, age) the image carries.
- Progress by code bytes and by number of functions, with the same figures and wording as
  `decomp status`: two bars with a segment per status (`library` and `skipped` included), a legend
  that names every segment, and the number of functions a live run is working on.
- Status buckets with functions, bytes and their shares: unstarted, in progress, non-matching,
  matched, refused, gave up, skipped, library; and the non-matching functions by best match in 10%
  bins, as a chart and as counts.
- A code map: a treemap with one cell per function of known size, grouped by section, sized by bytes
  and colored by status or by best match (a colorblind-safe ramp). The hovered or selected cell's
  function (name, address, size, status, best) is also written under the map, so nothing is shown
  only on hover.
- Progress over time: functions or bytes matched so far, per run or per local day, as a chart and a
  table (matched, new, cumulative, spend).
- Spend for the shown run and for all runs: dollars, tokens by type (input, output, cache write,
  cache read), the cache-hit rate, functions matched and dollars per match.
- Recent activity: the latest matches, give-ups, refusals and errors (the shown run's sessions and
  errors, then the outcomes the other runs' summaries record), newest first.

**Actions**

- Click a bar segment, legend entry, bucket or best-match bin to open the Function browser filtered to
  it; click a map cell, the entry point or an activity item to open that function's Inspector
  (right-click a function link for the other views).
- Export the progress report as Markdown or JSON.
- Color the map by status or best match; show progress per run or per day, in functions or bytes
  (remembered per project).

**Data sources**

- Project state: `decomp.json`, `symbols.txt` (statuses, best match, spend) and the target status
  (`Project::target_status()`); the loaded image for the identity and the map's sections.
- Run summaries (`.decomp/runs/*/summary.json`) merged with the shown run.
- Live: the live run's sessions overlay the stored states (a function being worked on counts as in
  progress, with its session's best match and spend: `vm::build_function_rows`).
- The figures are `vm::dashboard_progress`, `vm::build_text_treemap`, `vm::progress_history`,
  `vm::cost_report` (the same totals as the Cost view), `vm::recent_activity` and
  `vm::target_identity` (`src/viewmodel/`), computed as background jobs when the project, the program
  or the live overlay change (a few times a second at most during a live run).

**Notes**

Without a project, the Dashboard shows the shown run's spend and activity. The map is laid out in the
background (100,000 functions take about 60 ms in a Release build); the previous layout stays until
the new one is ready.

### Run monitor

**Shows**

- A worker table: worker, function, phase, turn `n/max`, elapsed time, best percentage, tokens and
  dollars, and the last tool call. A session's phase is one of: starting, waiting for model,
  thinking, writing, running `<tool>`, compiling, turn done, backoff, waiting for rate limit, waiting
  for approval, paused; an idle worker shows why it waits (waiting for the first session, paused, run
  budget exhausted, idle) or that it retired after concurrency was lowered.
- A plain-language live activity feed, for example "int __cdecl add(int, int): turn 2
  compile_and_diff -> compile: ok", with follow.
- The queue, with order, difficulty estimate and ETA. The difficulty estimate is the score the
  Function browser's difficulty column shows (size, blocks, loops, calls and unknown callees), and a
  run started here queues its functions easiest first by it. The workspace computes the scores in the
  background when the project opens and for each new program generation; a run started before they
  are ready uses the size alone. The ETA uses this project's observed session durations by size bucket (the last
  ten runs and the live run). A large run's queue arrives as its first 500 functions plus the number
  pending.
- A worker timeline: a Gantt chart of phases per worker, to spot bottlenecks such as long compile
  queues or backoff.
- Throughput: turns, compiles and retries per minute, output tokens per second, time to first token.
- Rate limits and retry history: the gate's latest snapshot (requests and tokens left, reset, backoff)
  and its history, and each retry with its status and delay.

**Actions**

- Reorder, pin, skip and remove queued functions; requeue finished ones.
- Change concurrency, the run budget and the per-function limits live.
- Pause or resume one worker (and the whole run from the top bar).

**Data sources**

- `worker_phase_changed`, `turn_started`/`turn_finished`, `tool_call_started`/`tool_call_finished`,
  `compile_started`/`compile_finished`, `diff_computed`, `retry`, `rate_limit_updated`,
  `queue_updated`, `budget_changed` and `control`, folded into `RunState` (workers with their phase
  spans, the queue, rate limits, per-minute statistics).
- The queue ETA from `vm::estimate_queue` (`src/viewmodel/eta.hpp`), which the shell computes once a
  second.

**Notes**

The progress view of `decomp run` (one line per worker) is the CLI version of this view
([CLI parity](#cli-parity)), and `decomp run --interactive` has its controls. The selected tab is
remembered per project.

### Agent session

**Shows**

- A header: the function (a link to the Inspector), the session id and a list of the run's other
  sessions; the status (running with its phase, or the outcome), model and effort, a "served by" badge
  when a fallback model answered a turn, and budget use: turns, dollars, tokens and time against the
  per-function limits, retries and the worker (amber from 80%).
- The timeline, turn by turn, read from the transcript (incrementally while the session runs):
  - each turn's time, stop reason, usage (input, output, cache write, cache read), dollars, time to
    first token and total latency, retries, and the serving model when a fallback served it;
  - thinking summaries, collapsed by default;
  - assistant text;
  - each tool call's input; a candidate source is syntax-highlighted and can be shown as a diff
    against the previous attempt;
  - each tool call's result: a diff summary, or compiler errors;
  - status lines, reminders, supervisor guidance (with its id) and pauses, inline where they were
    appended;
  - the turn being streamed (text and thinking) at the bottom, followed while it grows.
- Attempts: this session's score per attempt (chart), and the source history of every attempt for the
  function, with a "best" badge; attempts made by hand are marked.
- The agent's notes for the function.
- The outcome and its reason. For a refusal, the recorded category.

With no session chosen, the view follows the shared selection: the latest session of the selected
function, or the run's newest session when no function is selected.

**Actions**

- Send a guidance message (Ctrl+Enter). It is appended after the next tool results, which is safe for
  the append-only conversation. It shows as pending until then and can be retracted while pending.
- Pause or resume the session's worker; end the session (outcome `skipped`; its attempts and best
  source stay).
- Take over manually ([Manual mode](#manual-mode)).
- Open the session or any attempt in the Diff viewer.
- Compare any two attempts: a source diff, and both attempts' assembly diffs side by side, paired by
  target instruction (both are compiled again in the background).
- Export the transcript as recorded JSONL or as readable Markdown.

**Data sources**

- The transcript `.decomp/runs/<run-id>/sessions/<fn>[.<n>].jsonl`, read by `vm::TranscriptReader`
  (`src/viewmodel/transcript.hpp`): once for a past session, and only the appended bytes while a
  session runs (a rewritten transcript is read again from the start). Its `response` records carry
  the serving model, the thinking summaries and the usage of every turn.
- Live: the session's summary in `RunState` (turn, phase, scores, usage, cost, the streamed text and
  thinking of the current turn) from `stream_delta`, `turn_started`/`turn_finished`,
  `tool_call_started`/`tool_call_finished`, `diff_computed`, `guidance` and `session_finished`.
- The function's `attempts.jsonl` and `notes.md`.

**Notes**

Thinking content is the API's summary. The raw reasoning is never available, and an empty thinking
block shows as a "thinking (no summary)" marker. Only the visible part of the timeline is laid out, so
a transcript of several megabytes scrolls smoothly. Links can open a session on a given turn or tab.

### Diff viewer

**Shows**

- An objdiff-style aligned, side-by-side view of the target and candidate assembly for the selected
  function and attempt. Only the visible rows are drawn.
- Rows colored by kind and marked with a glyph: equal `=`, encoding `e`, operand `~`, opcode `!`,
  insert `+` and delete `-` ([matching.md](matching.md#5-row-kinds)), in the chosen diff palette. The
  differing operand is boxed within its row, with its category (register, immediate, memory, stack or
  symbol).
- Branch arrows in both gutters, offsets, and optionally the raw bytes and relocation markers.
- Symbol tooltips: what each operand refers to (address, demangled name, kind, size). The Row details
  panel shows the same for the current row, so nothing is shown only on hover.
- A header with the score, the exact and byte-exact flags, the counts per row kind, and how the source
  compiled (duration, cache hit).
- Panels: hints and binding suggestions, each linked to its rows; the data diff (the strings, floats,
  jump tables and switch index tables the function references, target against candidate); the
  current row in full; the compiler output.
- An attempt slider that scrubs through every attempt for the function (Best and Latest jump there),
  with each attempt's session, number and time, and whether it was made by hand.

**Actions**

- Toggles (remembered):
  - normalized or raw text;
  - show relocations;
  - raw bytes (Ctrl+B);
  - fuzzy registers and stack (show register-only and stack-only differences as equal);
  - differing rows only.
- Step through the differing rows (F7, Shift+F7); select rows and copy them as text, as
  `decomp diff` prints them.
- Accept a binding suggestion. This names the target's symbol with `source=user` and records it in
  provenance.
- Export the diff as text or JSON.
- Edit the source by hand, take over from the agent, verify and save, or hand back
  ([Manual mode](#manual-mode)).

**Data sources**

- `attempts.jsonl` (each attempt's source, score and `origin`, agent or user) and the best source.
  The shown attempt is compiled again in the background with the project's toolchain, flags and
  compile cache (the agent's compiles are usually cache hits) for its full diff.
- Manual compiles use the same compile and diff engine as the agent's tool, as background jobs of the
  GUI ([Manual mode](#manual-mode)).
- `SymbolDb` and the image for tooltips and the data diff.
- Without a project, the session's scores from `diff_computed`.
- The view shows the same report that `decomp --json diff` prints
  ([matching.md](matching.md#8-output-formats)).

### Function browser and inspector

**Shows**

- The Function browser: every function in a sortable, filterable, virtualized table. Columns:
  address, name, decorated name, size, status, best percentage, attempts, dollars spent, last attempt,
  symbol source, callers, callees, unknown callees, and complexity (blocks, loops, difficulty).
  Columns can be hidden and reordered (the decorated name and unknown callees start hidden). The live
  run's sessions overlay the stored states. A line above the table counts the functions shown and
  selected, and describes the filter.
- The Inspector for the selected function: a header (name, decorated name, range and size, status,
  best percentage, attempts and spend including a running session, which is linked) and tabs:
  - Disassembly: the annotated listing with block separators, loop depth bars and loop headers,
    labels, comments and optional bytes. Operands that reference something are links: a label scrolls
    to it, a function opens in the Inspector, data in the Binary explorer;
  - Cross-references: callers (also those that call through an incremental-linking thunk), callees,
    data references, and the data that stores the function's address (vtables, callback tables);
  - Attempts: the score of every attempt and the best so far (a chart), and every attempt with its
    time, match, session, author (agent or user) and summary;
  - Notes;
  - Status history: the status changes the runs recorded (time, from, to, best, run), and the
    function's sessions in the shown run.

**Actions**

- Filter by status, size range, name (text or regex), unknown callees, or refused; sort on several
  columns (Shift-click a header). Filters, sort keys and columns are remembered per project, and the
  Dashboard opens the browser with a filter.
- Multi-select (Ctrl and Shift clicks, Ctrl+A, box selection), then start a run over the selection (in
  table order) or add it to the live run's queue. The Inspector runs or queues its function.
- Mark functions skipped or library. Reset a status to unstarted; history is kept. A change to several
  functions, or to matched ones, asks first; skipping a function during a live run also takes it out
  of the run.
- Open a function in the Inspector (double-click or Enter), the Diff viewer, the Agent session, the
  Binary explorer or Symbols.
- Edit notes: one function's in a dialog or in the Inspector (add a note, edit, save or revert), or
  add a note to every selected function.
- Export the list, filtered and sorted as shown, as CSV or JSON.

**Data sources**

- `SymbolDb`; `symbols.txt` for status, best percentage, attempts, spend and source; the function's
  `attempts.jsonl` and `notes.md`.
- The code analysis (`analyze_functions()` in `analysis/difficulty.hpp`: callers, callees, unknown
  callees, blocks, loops and difficulty), which the workspace computes once per program generation
  and runs order their queue by, with its progress shown. The listing is
  `vm::build_listing` over `annotate_function`; cross-references are `vm::function_xrefs`
  (`Program::xrefs_to` and `xrefs_from`).
- The status history: the `status_changed` events in every run's `events.jsonl`
  (`vm::load_status_history`), read in the background.
- Live overlay: the live run's sessions (`vm::build_function_rows`).

**Notes**

Tables with 100,000 or more rows stay responsive: they use `ImGuiListClipper`, and rows, sorting and
filtering are built as background jobs (`vm::filter_and_sort`), the previous order staying shown
until the new one is ready.

### Binary explorer

**Shows**

- Sections: name, address, virtual and raw sizes, file offset, characteristics.
- Imports (DLL, name or ordinal, slot) with the calls through a selected slot, and exports (ordinal,
  name, address, forwarder).
- Strings (ASCII and UTF-16, from the data sections) with their section, reference counts and a text
  filter; the references to a selected string.
- A hex view per section with symbol overlays: functions, data, strings, floats, jump tables,
  relocations (underlined) and import slots, each color named in a legend; what starts on a row is
  written beside it. A selected byte shows what it belongs to and who references it.
- Classes named by the RTTI (`/GR` builds): each class with its direct bases (virtual ones marked), its
  type descriptor, and its vftables (for which base, when there are several) with every slot's
  function as a link.
- Rich header entries: product, build, count, the tool they name with its version and Visual Studio
  release, and whether the checksum matches; below them, what the target was built with and the
  notes of the toolchain suggestion.
- PDB information: match state, the PDB loaded, the path the image records, GUID and age.

**Actions**

- Go to an address or symbol; follow cross-references, and go back.
- Create, rename, edit or remove symbols (name, kind, size). They are recorded with `source=user`,
  appear in provenance, and the program is rebuilt with them.
- Jump to the function in the Inspector or the Diff viewer, or to the symbol in Symbols.

**Data sources**

- `pe::Image` and the PDB state, `SymbolDb` and analysis: the string scan (`scan_strings`) and its
  references (`string_refs`), the overlays (`vm::build_hex_overlays`) and the references to an address
  (`Program::xrefs_to`: the functions' references, pointers stored in data and references through
  linker thunks), each a background job per program generation; the Rich header's tools and versions
  (`pe::identify_build`); the classes and vftables (`Program::rtti`).

### Symbols and provenance

**Shows**

- Every symbol in a virtualized table: address, kind, demangled and decorated names, size, source,
  status (functions) and the number of recorded edits (marking agent edits). Columns can be sorted,
  hidden and reordered.
- For the selected symbol, who set it (its source: analysis, import, export, PDB, agent or user) and
  its recorded edits: when, by whom (with the session), what changed and why.
- Agent edits: every symbol change the agent made with `set_symbol`, grouped by session, each linked
  to its session (bindings recorded on a match are planned).

**Actions**

- Filter by name or address, kind, source, or edited symbols only (remembered per project).
- Create, rename, edit or remove a symbol (name, kind, size); removing one drops it from `symbols.txt`
  (what the binary itself says stays).
- Revert agent edits, one at a time or all of a session's: each symbol gets back what it had before,
  recorded as a user change. A revert is refused when a later change touched the symbol.
- Open a symbol in the Inspector, the Binary explorer, or the other views of a function.

**Data sources**

- `symbols.txt` for the current state.
- `.decomp/symbols.log.jsonl` for the history (every change made through `Project::set_symbol`: who,
  when, before and after, the reason and the session), and the shown run's `symbol_changed` events.
  Edits made to `symbols.txt` by hand are not logged.

### Changes and approvals

**Shows**

- The queue of gated actions waiting for a decision: today, verified matches waiting to be saved
  under the policy `ask`, each with its function, session, target path and a line diff against the
  file it would replace (or the new file).
- The live run's policy for each action type.
- Every file Decomp wrote into the project (`.decomp/changes.jsonl`, today verified sources): time,
  path, who wrote it (agent or user), the session and the reason (which says when the supervisor
  approved it), with a line diff of what it replaced (side by side, with line numbers and a "changes
  only" toggle).

**Actions**

- Approve or deny a waiting action, with a reason that a denied agent receives.
- Revert a change: the latest change of a file can be reverted while no run is live. The replaced
  content comes back from `.decomp/blobs/`, or a new file is removed (its function goes back to
  `nonmatching`); the revert is itself recorded.
- Set the policy for each action type for the live run: auto, ask or deny (Settings saves the
  project's default in `decomp.json`).

| Action type | Default policy |
|---|---|
| Write a verified source into its unit's source or `src/functions/` (`write_source`) | auto (mechanically verified) |
| Record symbol bindings from a match (planned) | auto (can be switched to ask) |
| Rename, create or resize a symbol through `set_symbol` | ask (deny in `decomp run`, where nobody can answer) |
| Add or replace a type in a shared header through `define_type` | ask (deny in `decomp run`) |

**Data sources**

- `approval_requested` and `approval_decided`, `file_written` (with size, SHA-1 and how it was
  approved), `control` (policy changes); the controller's pending approvals.
- `.decomp/changes.jsonl` and `.decomp/blobs/` ([project-format.md](project-format.md#changesjsonl-and-blobs)).

**Notes**

A gated action waits for its decision, and the worker shows the phase "waiting for approval"; Stop
does not end the wait, Abort does. A denial goes back to the agent as an error result with the
reason, so that it can adapt ([agent.md](agent.md#approvals)).

### Units

**Shows**

- The program's translation units in link order (`units.txt`): name, kind (`code`, `library`,
  `import`, `linker`), functions, functions matched, code bytes, the share of bytes matched, the
  spend on the unit's functions (their `cost=`), and the unit's source. A header says where the units
  came from (the PDB, the link map, `units.txt` or a guess of the analysis) and what was spent on them.
  Functions in no unit form a row of their own.
- For the selected unit: its kind, origin and source (and whether that is written yet), and its
  functions with address, name, status, best match and spend, each a link to the other views.
- The latest verification or emit of the unit: how many functions are byte-exact in its source, and
  which are not (or which kept their own files), with why.

**Actions**

- Run the unit's functions that are not finished (`decomp run --unit`), easiest first, or add them to
  the live run's queue.
- Verify the unit's source: compile it and diff every function it holds (`decomp units verify`), in
  the background.
- Emit: move the unit's matched functions' own files into its source (`decomp units emit`), keeping
  those that do not stay byte-exact there. Not during a run.
- Refresh, after `units.txt` was edited outside the GUI.

**Data sources**

- `units.txt`, each symbol's `obj=` and the function states in `symbols.txt` (`compute_unit_progress`),
  recomputed in the background when the project changes.
- The unit sources under `src/` and `.decomp/changes.jsonl` (what verify and emit write).

### Types

**Shows**

- The types the project's headers declare (`include/`), as the project's compiler lays them out:
  name, kind, size, the header, and whether the target's PDB has the same layout. Then the PDB's other
  types, marked as found only there. A header line counts them, and shows the compiler's errors when
  the headers do not compile.
- A filter by name, and All / In headers / Only in the PDB / Differ from the PDB.
- For the selected type: where it comes from, each difference from the PDB's layout, and its layout
  (offsets, bases, vfptr and vbptr, fields with their types and bits, virtual methods by slot) in a
  monospace block.
- Its uses, on request: every instruction of the program's functions that reaches one of its fields (or
  calls one of its virtual methods) through a pointer the function is given, with links to the
  function and the address (the same type flow as the annotated listings).

**Actions**

- Import from the PDB: declare the selected type, and what it needs, in `include/types.h`, checked
  against the PDB first (`decomp types import`). Import all from the PDB: every type `decomp types
  import --all` takes. Not during a run.
- Skeletons from RTTI, when the target's RTTI names classes: `decomp types skeletons`. Not during a
  run.
- Find uses of the selected type, in the background.
- Refresh, after the headers were edited outside the GUI.

**Data sources**

- The headers compiled and read back (`compile_header_types`), in the background, when the project
  changes; the PDB's layouts (`Program::pdb_types()`); the RTTI (`Program::rtti()`).
- What the actions write goes through `.decomp/changes.jsonl` like any project file, and shows in
  Changes.

### Relink

**Shows**

- The last relink ([project-format.md](project-format.md#relinking)): whether it is identical to the
  target, or how many bytes differ and where the first one is: its place (`.data+0x0`), its address,
  the unit whose contribution holds it and the symbol there. When it was made, how many units came from
  their sources, from split objects and from the linker, and whether the linker is the one that made
  the target (in warning colors when it is another version). The relink's notes.
- The units in link order: how each was linked (`source`, `split`, `linker`), its bytes, its source's
  check (`complete`, or what does not match) and why it was linked that way. Before the first relink,
  the units of `units.txt`.
- Comparison: the SHA-1s of the target and the relinked image (and of the image as the linker wrote it),
  the target's and the relinked image's bytes around the first difference side by side with the
  differing bytes highlighted, the sections with differing bytes (how many, the first, its unit,
  whether the section's size or place differs), the header fields that differ, and the fields taken
  over from the target (timestamps, the PDB's GUID and age, the checksum) with both values.
- Unit, for the selected unit: how it was linked and why, and its source's check section by section
  (the object's section, its symbol, where it was placed, its size, whether it is equal, differs,
  unplaced, or pooled or folded into another unit's or function), the unit's functions the source does
  not have, its contributions nothing fills, the problems the check found, and the compiler's output.
- Link: how long the link took, the linker's version line, the import libraries written, the command
  and the linker's output.

**Actions**

- Relink (`decomp relink`) in the background, with its current step shown, and cancel it. Every unit
  split (`--all-split`) tests the relink itself.
- Per unit, what the next relink does with it: as its check decides, from its source even when the
  check fails (`--source`), or from its original bytes (`--split`).
- Check units (`decomp units check`): compile every unit source and place it in the image, without
  linking; the units' checks show the result.
- Links: the first difference's address to the Binary explorer, a unit to its details or to Units, a
  function the source lacks to the Inspector.
- Refresh, after `decomp relink` ran outside the GUI.

**Data sources**

- `.decomp/relink/result.json` (`vm::read_relink_report()`), and the relinked image in
  `.decomp/relink/out/` with the fields taken over put back (`vm::load_stamped_relink()`) for the bytes
  around the difference (`vm::hex_compare()`); reread when the project changes or a relink ends.
- `units.txt` before the first relink; the unit checks of Check units, which replace the relink's own.

### Cost and usage

**Shows**

- Budgets: the live run's spend against its run budget (amber from the alert threshold, red at
  100%) and the per-function limits.
- Spend for all runs and this run, dollars and turns per match, tokens by type, the cache-hit rate,
  and the spend recorded per function.
- Tabs: spend by run (a chart and a table; a run opens read-only from it), by day (local days), by
  model and effort with success rates, by function, tokens by type per run (stacked), the cache-hit
  rate over the run and per run, and the projection for the functions still to do, from the
  observed spend per function by size bucket (buckets without history borrow the nearest one's,
  scaled by size).
- Flags for turns a fallback model served and for usage priced without a price-table row.

**Actions**

- Set the live run's budget and per-function limits, and the alert threshold (a GUI setting, 80% by
  default, which also colors the top bar and raises the budget warnings).
- Export the report as CSV (runs, days, models, functions) or JSON.

**Data sources**

- Run summaries (`summary.json` of every run, read in the background) merged with the live run's
  state; per-function spend from `symbols.txt` (`cost=`); `budget_changed`; the price table
  ([agent.md](agent.md#cost-accounting)). The report is `vm::cost_report` and
  `vm::project_remaining_cost` (`src/viewmodel/cost.hpp`), recomputed in the background when the
  runs, the project or (every two seconds) the live run change.

### Toolchains and compiles

**Shows**

- The registry: name, kind, compiler path, wrapper, flags, include directories and environment,
  for the user's toolchains and the detected clang-cl ones.
- What the open project's target was built with (from its Rich header), and whether a toolchain for
  that release is configured.
- Health-check results: whether a probe compiles into a usable object, the command line and output,
  the compiler's version (cl.exe's banner, or `--version`) and, for the open project, how the
  toolchain fits the target's compiler: the same build, the same release with another build, another
  release, or unknown (no compiler ID in the probe object, as with clang-cl).
- The run's recent compiles (the last 200): time, function, toolchain, result, exit code, duration,
  cache hit, and for the selected one the full command line and output.

**Actions**

- Add, edit or remove toolchains (this writes the user-level registry); save a copy of a detected
  one.
- Run a health check.
- Re-run a recorded compile from its attempt's source with the project's toolchain, bypassing the
  compile cache, and see its diff summary and output.

**Data sources**

- `ToolchainRegistry`, `matching::check_toolchain` and `detect_version`; the target's
  `pe::BuildInfo`, `matching::suggest_toolchain` and `matching::toolchain_fit`.
- `compile_started` and `compile_finished` (toolchain, command line, exit code, output up to 4 KB,
  duration, cache hit), and the function's `attempts.jsonl` for a re-run.

### Logs and errors

**Shows**

- Errors grouped by kind: API errors and retries (with status and delay), compiler failures, tool
  errors, failed sessions, and warnings and errors logged during the run (300 kept).
- The run's log (warnings and errors logged during it, 500 kept) and this application's own log, with
  a level filter and search.

**Actions**

- Copy a line or the shown lines.
- Open the session an entry belongs to.

**Data sources**

- `log` events, `retry`, `tool_call_finished` with `is_error`, `compile_finished` failures and
  `session_finished` with outcome `error`, kept as structured records by `RunState`; the
  application's in-memory log (`log::recent()`).

### Settings

**Shows**

- API key status: whether `ANTHROPIC_API_KEY` is set, and a Check that lists the models the key can
  use (a `GET /v1/models` request, which consumes no tokens). The key itself is never shown and cannot
  be entered here; it comes from the environment.
- The project's agent settings (`decomp.json`): model, effort, fallbacks, workers, run budget,
  per-function turns, dollars, tokens and minutes, and the approval policy for saving verified
  matches; the price table.
- Appearance: theme, font size, colorblind-safe diff palettes.
- Recent projects and saved layouts.
- Developer: a replay directory that drives sessions from scripted responses, so runs need no key.

**Actions**

- Edit the project's agent settings and save them to `decomp.json` (shared through git), or revert
  the edits. GUI settings (appearance, layouts, recent projects, the budget alert, the replay
  directory) are saved per user in `gui.json`.

**Data sources**

- `decomp.json`, the user's `gui.json`, and the price table in `agent/cost.hpp`.

### Later-phase views

| View | Phase | Shows |
|---|---|---|
| Permuter and flag search | 6 | Search runs, candidates tried, best score over time, and the winning flags or permutations |

## Interactions

### Run control

| Command | Effect | Takes effect |
|---|---|---|
| Start | Builds the queue from the selection and starts the workers | Immediately |
| Pause | Each worker finishes its current turn (request and tools), then waits | Between turns |
| Resume | Workers continue | Immediately |
| Stop | Workers finish their current turn, then sessions end with outcome `stopped` and best attempts are kept | Between turns |
| Abort | In-flight requests and running compiles are cancelled and partial turns discarded; sessions end with outcome `aborted` | Immediately |
| Skip function | Ends that function's session, if any, after its turn (outcome `skipped`), or takes it out of the queue | Between turns |
| Pause worker | Pauses one worker | Between turns |
| Set concurrency, change the run budget or per-function limits | Applied to the running run | Next scheduling decision (limits: the sessions' next check) |
| Requeue, enqueue, remove, move, pin | Changes the queue | Next scheduling decision |

The GUI sends these through `RunCommands` (the top bar, the Run monitor, the Function browser, the
Agent session); `decomp run --interactive` has the same commands on stdin (`:pause [worker]`,
`:resume [worker]`, `:stop`, `:abort`, `:skip`, `:requeue`, `:workers`, `:budget`, `:guide`), and Ctrl+C
stops (twice: aborts). A single `decomp agent <func>` session has Pause, Resume, Stop and Abort (Ctrl+C,
and `:pause`, `:resume`, `:stop` and `:abort` with `--interactive`).

A turn is never left half-recorded. A paused session resumes exactly where it was, and a stopped
session can be inspected exactly as it was through its transcript. A stopped, budget-limited or
interrupted run can be resumed: its unfinished functions start again with a fresh conversation whose
brief carries their attempts, notes and best source ([agent.md](agent.md#resuming)).

### Steering

The Agent session view has a guidance composer. A message is queued for the session and appended to
the conversation at the next safe point: in the next user message, after the tool results (or the
nudge) and before the status line, prefixed `[Supervisor guidance]`. Until then it shows as pending
and can be retracted. Once appended, it is part of the conversation for good (the conversation is
[append-only](agent.md#the-append-only-conversation)) and appears inline in the timeline. Guidance can
also carry a source: the Diff viewer's "hand back" sends the edited source this way. From the CLI,
`decomp agent --interactive` queues each line typed on stdin as guidance, `--guidance` queues text for
the first request, and `decomp run --interactive` takes `:guide <fn> <text>`; a `guidance` event and
transcript record mark when it was sent.

### Approvals

Gated actions appear in Changes & approvals and as notifications. A decision applies immediately.
The policy for an action type can be changed from the same place for the live run (Settings changes
the project's default), and changes are recorded as `control` events, so the audit trail shows who
allowed what; `approval_decided` records each decision, and `.decomp/changes.jsonl` how each write was
approved ([agent.md](agent.md#approvals)).

### Manual mode

"Take over" (Agent session, Diff viewer) pauses the agent's session for that function, so that the
user can hand back, or ends it (outcome `skipped`), then opens the Diff viewer editing the best
attempt; "Edit by hand" does the same for a function without a session. The editor
(ImGuiColorTextEdit) sits beside the diff. A compile starts once the text has been quiet for half a
second and runs in the background; the latest edit wins, and the diff is marked stale until it has
caught up. Diagnostics are listed under the editor and jump to their line. Any attempt can be loaded
into the editor.

"Verify and save" (Ctrl+S) runs the same mechanical check as `submit_result` (compile, diff, require
`byte_exact`). Every verification is recorded in `attempts.jsonl` as an attempt with `origin: user`,
in a session of its own (`user-<time>`), and a better score updates the best source. A byte-exact one
is saved (recorded in `.decomp/changes.jsonl` as written by the user, "verified by hand") and the
function becomes `matched`. It is saved where a session would save it
([agent.md](agent.md#translation-units)): into the function's unit source, when every function of the
unit that was byte-exact there stays so, else to `src/functions/`. The editor's compiles work the same
way: in a unit, the source is composed into the unit's source and the whole unit is compiled.

"Hand back" sends the edited source to the agent as guidance: to the function's live session (a
session paused for the take-over goes on); else the function is queued again in the live run and the
source follows when its session starts; else a run on that function starts with the source as its
first guidance.

Manual compiles use the project's toolchain, flags and compile cache, and the same compile and diff
engine as the agent's tool. They are background jobs of the GUI, not run events. Candidate code is
only compiled, never executed.

### Navigation

- Every function name, address, symbol, attempt and turn is a link.
- Context menus offer "open in" any view that can show the item.
- Back and forward history covers all navigation.
- Selection is shared: selecting a function in one view updates the Inspector and any view that
  follows the selection.

### Search and command palette

One input (Ctrl+P) searches:

- functions and symbols by readable or decorated name (fuzzy, ranked like the actions; functions come
  first among equals);
- addresses (typing `0x401000` jumps there);
- strings in the image (prefix `"`);
- actions (fuzzy; prefix `>` for actions only), such as "Start run", "Open project..." or "Reset
  layout".

Every action in the UI is reachable from the palette. The names and strings are indexed in the
background for each program generation, and each query runs as a latest-wins job over the index
(`src/gui/views/palette_search.hpp`, `src/viewmodel/search.hpp`), so typing never waits for a search
of 100,000 names.

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
| Budget at the alert threshold, 80% by default (run or function) | Warning | Yes | Cost and usage |
| Budget at 100% | Error | Sticky | Cost and usage |
| Authentication error (401 or 403) | Error | Sticky | Settings |
| Rate-limit storm (many 429s in a short window) | Warning | Yes | Run monitor |
| Toolchain failure (compiler missing, health check failed, compile crash or timeout) | Error | Sticky | Toolchains and compiles |
| Run finished, with a summary (matched, gave up, refused, spend, duration) | Info | Yes | Dashboard |
| Approval requested | Warning | Yes | Changes and approvals |
| A fallback model served a turn | Info | No, history only | Agent session |

The rules are `vm::NotificationRules` (`src/viewmodel/notification_rules.hpp`), applied to every new
snapshot. A notification is keyed by what the run recorded (session, approval id, event time), so a
repeated or replayed snapshot and a resumed run never repeat one, and a past run opened after
`prime()` posts nothing. Three or more per-function notifications of one kind within 1.5 seconds become
one summary. Detection is limited to what the snapshot records: an authentication error is a 401 or
403 (or a missing key) in a session's error or in the log; a storm is five or more retries after a 429
in two consecutive minutes; a toolchain failure is a compile that timed out, crashed, wrote no object,
or failed without a single diagnostic, or a compile tool call that failed because the compiler could
not be started. A failed health check in the Toolchains view posts its own sticky error. The shell
primes the rules with a resumed or reopened run's history, so only what happens while it watches is
announced; toasts link to the view named in the table.

## Accessibility

- **Colorblind-safe palettes.** The default diff palette is chosen to stay distinguishable under the
  common color-vision deficiencies. Alternative palettes and a high-contrast theme are in Settings.
- **Never color alone.** Each row kind also has a gutter glyph: `=` equal, `e` encoding, `~` operand
  (`@` when a symbol differs), `!` opcode, `+` insert, `-` delete. The differing operand is boxed as
  well as colored, and run states and health lights carry text.
- **Font scaling.** Fonts scale at runtime (Ctrl+= and Ctrl+-) and follow per-monitor DPI. The
  embedded fonts are rasterized at the sizes in use.
- **Keyboard navigation.** ImGui keyboard navigation is on, every action is reachable through the
  palette, and nothing is available only on hover: tooltips are mirrored in the Inspector or a details
  pane.
- **Reduced motion.** There are no animations. Following live output (auto-scroll) can be turned off
  per view.
- **Screen readers.** Dear ImGui exposes no accessibility tree, so screen readers cannot read the GUI.
  The CLI is the accessible path for monitoring: `decomp status`, the progress view in plain-line
  mode (used whenever stderr is not a terminal), `--json` output and the JSONL transcripts.

## Persistence

| What | Where |
|---|---|
| Window and dock layout, named saved layouts, recent projects, theme, font size, diff palette, budget alert threshold, replay directory | User config directory (`%APPDATA%\decomp\` or `~/.config/decomp/`): `gui.json` and `imgui.ini` |
| Per-project view state (open views, filters, column layout, sort order, selected tabs) | `gui.json`, keyed by project path |
| Agent settings, budgets, workers and approval policies chosen per project | `decomp.json` (shared through git) |
| Exports (reports, lists, transcripts, diffs) | `.decomp/exports/` |
| Runs, transcripts and history | Already on disk under `.decomp/`. The UI only reads them. |
| Anything about the API key | Nowhere |

## CLI parity

The CLI covers inspecting the target, matching by hand, and running, watching and steering agent
sessions, one (`decomp agent`) or many (`decomp run`). `--json` is a global option, given before or
after the command name (`decomp status --json`), and most commands honor it; `toolchain add` prints
text only. Approvals that wait for a person (`ask`), manual mode and browsing past runs view by view
are GUI features; the CLI takes the policies `auto` and `deny`, and `decomp runs show` summarizes a
past run.

| GUI | CLI |
|---|---|
| Dashboard | `decomp status`: functions and code bytes matched, status buckets, spend (the sum of the functions' `cost=` in `symbols.txt`), and per unit: functions and bytes matched and spend (`units` in its JSON) |
| Units | `decomp units` (every unit with its kind, progress, spend and source; `derive`, `verify`, `emit`), `decomp run --unit <name>` |
| Types | `decomp types` (the headers' types and whether the PDB agrees; `--pdb` for the PDB's), `decomp types show <name>`, `decomp types check`, `decomp types import`, `decomp types skeletons`; uses of a type's fields are a GUI feature, and `decomp disasm` names the fields in a listing |
| Relink | `decomp relink` (`--source`, `--split`, `--all-split`; the result in `.decomp/relink/result.json`, `--json` prints it), `decomp units check`, `decomp units compose`; the bytes around the first difference side by side are a GUI feature |
| Run monitor | `decomp run` and `decomp agent <func>`: a live progress view on stderr, on by default (`--no-progress` hides it, `--progress` keeps it with `--json` or `-q`). On a terminal it is a block redrawn in place: a run header (run ID, status, model and effort, elapsed time, spend, cache-hit rate, functions matched), the queue length, one line per worker (function, turn, phase, best score, spend, elapsed time; at most twelve) and the last four activity lines. Otherwise it prints the activity lines as they happen. `decomp run --interactive` takes the run controls on stdin. |
| Agent session | Steering with `--interactive` (guidance lines and `:pause`, `:resume`, `:stop`, `:abort` for `decomp agent`; `:guide <fn> <text>` and the run controls for `decomp run`) and `--guidance`; Ctrl+C to stop, twice to abort; the transcript in `.decomp/runs/<run-id>/sessions/<fn>.jsonl` |
| Diff viewer | `decomp diff <func> --source <file>` or `--obj <file>` (with `--compact`, `--context`, `--bytes`) |
| Function browser | `decomp funcs` (address, size, symbol source and name; `--filter <regex>`); statuses, scores and spend are in `symbols.txt` |
| Binary explorer | `decomp info`, `decomp disasm <func>` |
| Toolchains and compiles | `decomp toolchain list`, `decomp toolchain test <name>` (with the compiler's version), `decomp toolchain add <name>` |
| Past runs | `decomp runs list`, `decomp runs show <id>` (equal to its `summary.json`), `decomp run --resume <id>` |

## Architecture

Sessions publish events from their worker threads; the bus delivers them one at a time, in sequence
order, to every subscriber. The GUI never reads worker state: it renders immutable snapshots and sends
commands to the run controller.

```
 RunController: N workers (decomp agent: one), one session each (run_loop, tools, compiles)
        | publish(Event)
        v
    EventBus (serialized) --+--> JsonlEventLog --> .decomp/runs/<run-id>/events.jsonl   (no stream deltas)
                            +--> ProgressRenderer --> stderr                            (CLI)
                            +--> RunStateStore (reducer) --> snapshot(): shared_ptr<const RunStateData>
                                                                 | once per frame
                                                                 v
                                                          decomp-gui views
                                                                 | RunCommands
                                                                 v
                                                          RunController --> LoopControl per session

 decomp run and decomp agent: --interactive, Ctrl+C --> RunController
```

### Events

Events are a `std::variant` of plain structs (`src/events/events.hpp`). Each one carries a header
with a sequence number (`seq`), a UTC time (`time`, milliseconds since the Unix epoch), the run ID
(`run`) and, where it applies, the worker (`worker`); session-related payloads carry the session ID.
In `events.jsonl` an event is one line with its `type` and its payload under `data`:

```json
{"data":{"best":100.0,"function":"?add@@YAHHH@Z","old_status":"unstarted","status":"matched","va":4198496},"run":"2026-10-04T02-08-33-f11d","seq":47,"time":1791079713812,"type":"status_changed","worker":0}
```

| Type | Payload |
|---|---|
| `run_started` | `project`, `model`, `effort`, `workers`, `functions` (the selection's names), `vas`, `config` (budgets, limits, policies) |
| `run_finished` | `status`: `completed`, `stopped`, `aborted`, `budget_exhausted` or `error` |
| `run_resumed` | `interrupted`: the functions whose sessions were cut off and start again |
| `session_started` | `session`, `function`, `display`, `va`, `transcript` (relative to the run directory) |
| `session_finished` | `session`, `outcome`, `detail`, `best_match`, `turns`, `cost_usd` |
| `turn_started` | `session`, `turn` |
| `turn_finished` | `session`, `turn`, `stop_reason`, `usage` (`input`, `output`, `cache_write`, `cache_read`), `cost_usd`, `latency_ms`, serving `model`, `ttft_ms`, `had_fallback` |
| `stream_delta` | `session`, `kind` (`text` or `thinking`), `text` |
| `tool_call_started` | `session`, `id`, `tool`, `turn`, `input` (a preview: long strings are shortened) |
| `tool_call_finished` | `session`, `id`, `tool`, `is_error`, `summary` (the first line of the result), `duration_ms` |
| `compile_started` | `session`, `toolchain`, `command` |
| `compile_finished` | `session`, `ok`, `cached`, `duration_ms`, `errors`, `exit_code`, `command`, `output` (at most 4 KB), `toolchain` |
| `diff_computed` | `session`, `match_percent`, `byte_exact`, `summary`, `attempt`, `rows` (`equal`, `encoding`, `operand`, `opcode`, `insert`, `delete`) |
| `retry` | `session`, `attempt`, `error`, `delay_ms`, HTTP `status`, `retry_after_ms` |
| `refusal` | `session`, `category`, `explanation` |
| `guidance` | `session`, `text`, `id` |
| `status_changed` | `function`, `va`, `status`, `old_status`, `best` |
| `file_written` | `path`, `reason`, `size`, `sha1`, `session`, `approval` |
| `log` | `level`, `message`, `session` (warnings and errors logged during a run) |
| `worker_phase_changed` | `phase` (for example `thinking`, `compiling`, `waiting for rate limit`, `waiting for approval`, `paused`, `idle`, `retired`), `session`, `function` |
| `rate_limit_updated` | `requests_limit`, `requests_remaining`, `input_tokens_limit`, `input_tokens_remaining`, `output_tokens_limit`, `output_tokens_remaining`, `reset`, `backoff_ms` |
| `budget_changed` | `scope` (`run` or `function`), `usd`, `tokens`, `turns`, `minutes` |
| `symbol_changed` | `va`, `old_name`, `new_name`, `kind`, `size`, `source`, `session` |
| `approval_requested` | `id`, `action`, `session`, `function`, `va`, `path`, `summary` |
| `approval_decided` | `id`, `verdict` (`approved`, `denied`, `cancelled`), `by` (`policy` or `user`), `reason` |
| `queue_updated` | `items` (the first pending functions in dispatch order, at most 500: `va`, `function`, `pinned`, `difficulty`, `sessions`), `total` (all pending functions) |
| `control` | `command`, `target`, `detail`: a supervisor command, once applied |

Fields added after the first slice have defaults, so a slice-era `events.jsonl` still replays.

### RunState

`RunState` (`src/events/run_state.hpp`) is a pure fold over events: `RunState::apply(const Event&)`
contains no I/O and takes times only from the events. `RunStateData` holds:

```
run         id, project, model, effort, status, worker count, planned functions (names and addresses),
            config, interrupted functions, started, ended
sessions{}  function, display, va, worker, phase, turn, tool calls, compiles and errors, best and last
            match, matched, scores[] (one per diff), usage, cost, retries, last tool and its summary,
            stream tail, the current turn's streamed text and thinking (16 KB each, cleared every
            turn), serving model, time to first token, fallback turns, refusal category, transcript,
            outcome, detail, started, ended
workers{}   session, phase, function, phase start, phase spans (the worker timeline, 1,000 per worker)
queue       the head of the pending queue (at most 500) and the number pending
approvals   by id, with the number pending
rate_limit  the latest snapshot of the rate gate, and a history (240)
budget      run and per-function limits
totals      tokens by type, dollars, matched, finished, retries, refusals, tool calls, compiles,
            fallback turns, cache-hit rate
activity    the last 200 plain-language lines, such as
            "[02:08:33] int __cdecl add(int, int): turn 2 compile_and_diff -> compile: ok"
errors      the last 50 error lines, and 300 structured records by kind (api, tool, session,
            compiler, log)
files       files written, recent compiles (200, with command and output), the log tail (500),
            supervisor commands, symbol changes
minutes     per-minute activity for throughput charts (turns, tokens, cost, compiles, retries, time
            to first token; 1,440 minutes)
```

Everything is capped, so a long run uses bounded memory. View models (`src/viewmodel/`) derive
tables, chart series and ETAs from `RunStateData`; they are pure and tested without ImGui.

### Snapshots

`RunStateStore` keeps the reducer behind a lock and hands out immutable snapshots
(`std::shared_ptr<const RunStateData>`). A snapshot is cached until the next event, so a UI that
redraws without new events gets the same one for free. Taking a snapshot copies the run's small
fields and shares the rest: sessions are `shared_ptr`s, and the reducer clones a session only when an
event changes one that a snapshot still holds (copy-on-write by generation); the planned functions,
the queue and the recent compiles and log lines are shared, immutable vectors or records. The GUI takes
one snapshot per frame; workers wake the UI loop, at most once per frame, when events arrive. With
1,000 sessions a snapshot takes well under a millisecond in a Release build (the performance test in
`tests/unit/perf_tests.cpp` measures it).

### RunController

`RunController` (`src/run/controller.hpp`) is the only way to change a run: start (a selection and
its settings), resume, pause and resume (all workers or one), stop, abort, skip, requeue, enqueue,
remove, move, pin, concurrency, the run budget, per-function limits, guidance (with retract), approval
decisions and policies. Every command is applied under the controller's lock, forwarded to the
affected sessions' `LoopControl`, and acknowledged by a `control` event; the views show the result
from the snapshot. The GUI reaches it through `RunCommands` (`src/gui/services.hpp`); `decomp run
--interactive` maps its stdin commands onto it, and so does `decomp agent`, a one-function run.

### Replay of past runs

Opening a past run reads its `events.jsonl` through the same reducer and shows the result in the same
views, read-only, with the run controls disabled. Transcripts are loaded on demand when an Agent
session is opened. "Run again" starts a new run with the same selection and configuration, and
"Resume" continues a run that stopped, ran out of budget or was interrupted. Because past and live runs
share one code path, anything visible live is visible afterwards; the one gap is the streamed text,
which `events.jsonl` omits and the transcript holds in full.

## Testing strategy

- **Reducer unit tests.** Synthetic event sequences fold into the expected `RunState`: sessions,
  workers, the queue, approvals, rate limits, budgets, compiles, errors and the activity feed
  (`tests/unit/events_tests.cpp`, `events_store_tests.cpp`).
- **Log round trip.** Events are written to JSONL, read back and replayed, and must produce the same
  `RunState` apart from the streamed text, which the log omits; a slice-era log still replays; a live
  run's state equals its replay.
- **Progress view tests.** The rendered status block and the plain-line output are checked for a
  scripted run.
- **View-model tests.** Pure derivations (table rows, feed sentences, chart series, ETAs, costs,
  notifications, exports) are tested without ImGui: `src/viewmodel/` and
  `tests/unit/viewmodel_*_tests.cpp`, including transcripts written by the real runner and
  100,000-function tables and treemaps.
- **Headless GUI tests** (`decomp_gui_tests`). An ImGui context with no window or GPU backend renders
  the shell and every view for several frames against synthetic snapshots (no run, empty, mid-stream,
  error), against a real project with a live, finished and reopened run driven by fake sessions, and
  every tab of the views with tabs. ImGui assertions are turned into test failures, and every item is
  checked for conflicting IDs. This runs in CI on Linux and Windows; Linux CI also runs `decomp-gui`
  under Xvfb through a whole scripted run and keeps its screenshot.
- **Performance checks.** In Release builds, folding events, taking snapshots and replaying a log of a
  run over 100,000 functions with 1,000 sessions (`tests/unit/perf_tests.cpp`), and drawing every view
  for that run (`tests/gui/perf_tests.cpp`), print their times and fail only at five times their budget.
- **Manual acceptance.** The Phase 1 exit scenario, on Windows and Linux
  ([acceptance.md](acceptance.md)).

## Phasing

| Part | Phase |
|---|---|
| Event types, `EventBus`, `RunState` reducer, JSONL log and replay, CLI progress view, single-session `LoopControl` | First slice |
| `decomp-gui`: chrome and every view above except the later-phase ones; multi-worker `RunController` (queue, concurrency, shared rate limiter, approvals queue, live budget changes, resumable runs) | Phase 1 |
| Binary explorer enrichment (full cross-reference index, Rich-header compiler names, RTTI class names) | Phase 2 |
| Units view; approvals for `set_symbol` and `define_type` | Phase 3 |
| Types view; field names in listings | Phase 4 |
| Relink view | Phase 5 |
| Permuter and flag-search view | Phase 6 |

Phase 1 is done when, on Windows and Linux, a user can open a project, run 20 or more functions on 4
workers, watch live sessions, steer one, pause and stop, review diffs and changes, and reopen the
finished run from its log ([roadmap.md](roadmap.md#phase-1-supervision-gui-and-batch-runner)).
