# Phase 1 acceptance

The manual check of the [Phase 1 exit criteria](roadmap.md#phase-1-supervision-gui-and-batch-runner),
done by a person on Windows (the primary platform) and on Linux (X11, or XWayland on a Wayland
desktop). Automated tests cover the same paths with scripted sessions; this checklist adds the real
API, a real target and a person at the controls.

## What you need

- A Release build of `decomp`, `decomp-gui`, `decomp_tests` and `decomp_gui_tests`, with both test
  binaries passing.
- A project for a real target (`decomp init <binary> --toolchain <name> --flag ...`) whose toolchain
  passes `decomp toolchain test <name>`, with at least 25 functions that are not matched yet.
- `ANTHROPIC_API_KEY` set in the environment that starts `decomp-gui`. A run budget of a few dollars
  is enough for the whole checklist.

Without a key, the same steps work with scripted sessions: set Settings > Developer > replay directory
to `tests/replay/run` and use a project made from `tests/fixtures/x86/basic.exe` (13 functions, so
run all of them instead of 20).

## Checklist

### Open a project

- [ ] `decomp-gui --project <dir>` (or File > Open project) opens the project. The Dashboard shows
      the target's identity with the SHA-1 verified against `decomp.json` and the PDB state, and
      progress numbers equal to `decomp status`.
- [ ] Settings shows the API key as present (never its value); Check lists the models.
- [ ] Toolchains and compiles: the project's toolchain passes its health check and shows its version.

### Run 20 or more functions on 4 workers

- [ ] In the Function browser, filter the functions that are not matched, select 20 or more and
      queue them (or Run > Start run for the default selection), with 4 workers and a run budget.
- [ ] The first session starts alone; the others start once it streams its first response. From
      their first turn, later sessions read the prompt prefix from the cache (Cost and usage: cache
      hits; the transcripts' `cache_read_input_tokens`).
- [ ] The top bar shows the run state, spend against the budget and the API light; the status bar
      shows progress, the queue and an ETA.

### Watch live sessions and steer one

- [ ] Run monitor: four workers with their phases, turns and spend; the activity feed; the queue
      with ETAs; the worker timeline; throughput; rate limits.
- [ ] Agent session: open a live session. Thinking summaries and text stream in; each turn shows
      its tool calls, compiles and diffs; the score chart grows.
- [ ] Send guidance: it shows as pending, then appears inline after the next tool results. Retract a
      second one before it is sent: it never appears.
- [ ] Lower the per-function turn limit (Run monitor > Limits): the next status line reports it.
- [ ] Skip a queued function and requeue a finished one.

### Pause and stop

- [ ] Pause one worker, then the whole run, then resume both. Paused workers finish their current
      turn first.
- [ ] Stop the run: running sessions finish their current turn and end `stopped`; the queue keeps
      the rest.

### Review diffs and changes

- [ ] Diff viewer: a function's best attempt as an aligned diff, with row kinds, operand highlights
      and the attempt slider.
- [ ] Set the write policy to "ask" (Changes and approvals) and resume the run: a verified match
      waits with its diff; approve one, deny another with a reason (the agent's next turn shows the
      reason).
- [ ] Changes and approvals lists the approved write ("approved by user"); revert it: the source
      file goes away and the function is back to `nonmatching`.
- [ ] Take over a function (manual mode): edit the source, the diff recompiles in the background,
      Verify and save writes it and marks the function matched.

### Reopen the finished run from its log

- [ ] Let the run finish (or stop it), close `decomp-gui`, start it again: Run > Runs lists the run;
      open it read-only. Every view shows the same data as during the run (the streamed text is in
      the transcripts), and nothing is announced again as new.
- [ ] Resume a stopped run: the rest of the queue runs, and the views show the whole run.
- [ ] `decomp runs show <id>` agrees with the run's `summary.json`, and `decomp status` with the
      Dashboard.

## Recording the result

Note the platform, the target, the run id and anything that did not behave as described, with the
run directory (`.decomp/runs/<id>/`: `events.jsonl`, `sessions/`, `summary.json`), which holds
everything needed to investigate.
