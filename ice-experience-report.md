# ICE Workflow Report: RLC Circuit + Simulation Showcase

Date: 2026-10-30
Project: `untitled`
Scope: building a series RLC schematic, running an ngspice transient simulation,
creating graphs, and writing a Python showcase script.

## Summary

The core flow works: a text schematic can be simulated, results are stored
reliably, and the Python graph/showcase API is pleasant once it is running.
Most friction came from **tooling integration and setup**, not from the
simulation engine itself.

The biggest issues were:

1. The Python report runner was missing from the install, so every script failed.
2. `ice-mcp` and `ice-cli` can hold conflicting project state; CLI edits did not
   appear in MCP until the project was reopened.
3. Only node-voltage signals are captured, and unsupported signals are dropped
   silently unless *all* requested signals are missing.
4. There is no way to update a text schematic in place.
5. `script-run` prints the full graph data arrays, which floods the terminal.

Everything below includes what happened, why it matters, and a concrete
suggestion.

---

## 1. Python report runner is not installed (high impact)

**Observed**

```
$ ice-cli script-run ... probe
error[report.runner]: Python report runner is not available
```

The runner existed in the source tree at
`assets/simulation/report_runner.py`, but the installed binaries under
`~/.local/bin` could not find it. `reportRunnerScriptPath()` searches:

- `<exe>/../../../.../assets/simulation/report_runner.py`
- `<exe>/../share/ice/assets/simulation/report_runner.py`
- ancestors of the current directory
- `/usr/share/ice/...` and `/usr/local/share/ice/...`

None existed for the installed prefix, so simulation scripts were unusable out
of the box. The fix required manually copying:

- `assets/simulation/report_runner.py` -> `~/.local/share/ice/assets/simulation/`
- `python/icepy_sim.py` -> `~/.local/share/ice/python/`

**Why it matters**

This is the feature used for graphs and showcase reports. A missing file turns a
documented workflow into a dead end.

**Suggestions**

- Install the Python assets and `icepy_sim.py` as part of the normal install
  (packaging step or `ice-pkg`).
- Improve the error to list the paths that were searched and the exact fix,
  e.g. `Set ICE_REPORT_RUNNER=... or install assets under ~/.local/share/ice/`.
- Add a `ice-cli doctor` command that checks for the runner, Python, ngspice,
  and asset directories.

Reference: `src/icelib/simulation_report.cpp:326` and `:438`.

---

## 2. MCP and CLI share a project file but not project state (high impact)

**Observed**

- The project was opened through `ice-mcp`.
- `ice-cli simulation-remove` and `ice-cli schematic-remove` succeeded on disk.
- `ice-mcp` still reported the deleted schematic, and
  `ice-mcp schematic_create_text` then failed with
  `A schematic named 'rlc' already exists`.
- Reopening the project with `ice-mcp project_open` was required to resync.

**Why it matters**

Two clients editing the same project can silently diverge. Because successful
mutations are auto-saved, an MCP write based on stale state can overwrite or
resurrect CLI changes. This is a data-loss risk, not just an inconvenience.

**Suggestions**

- Detect external changes before every mutation (mtime/hash of
  `project.icepro` and cell files) and reload or refuse with a clear message.
- Add an explicit `project_reload` / `project_sync` operation.
- Or add file locking with a "project is open in another client" warning.
- Make the MCP tool surface complete so one client can own the whole workflow
  (see item 3).

---

## 3. `ice-mcp` has no simulation create/run tools (medium impact)

**Observed**

`ice-mcp` exposes simulation graph CRUD and script CRUD/run, but not:

- create a simulation (`simulation-add`)
- run a simulation (`simulation-run`)
- update `signals` / stop / step after creation

So the RLC flow had to switch to `ice-cli` for the most important steps, which
directly caused the stale-state problem in item 2.

**Suggestions**

- Add `simulation_create`, `simulation_update`, and `simulation_run` to MCP.
- Return a compact run summary plus run id, and provide a separate tool to fetch
  traces/graphs.

---

## 4. Only voltage traces are captured; unsupported signals are dropped silently (high impact for analog work)

**Observed**

The simulation was created with:

```
signals = "v(in),v(n1),v(n2),i(v1)"
```

The stored run contained only `v(in)`, `v(n1)`, `v(n2)`. `i(v1)` disappeared
without a warning. Source inspection shows why: every requested signal is
normalized to a node-voltage vector name:

```cpp
// src/icelib/simulation.cpp:123
// A signal may be given as "out", "v(out)", "N15" or "v(N15)" ...
const auto voltageVectorName = [](const std::string &signal) {
    ...
    return "v(" + name + ")";
};
```

`missing` is collected, but only an all-missing request is treated as an error
(`simulation.cpp:175`). A partial miss is silent.

**Why it matters**

Current is essential for RLC/power work. The workaround was to derive
`i = (v(in) - v(n1)) / R` in the report script. That is valid, but it should be
an explicit choice, not a surprise.

**Suggestions**

- Support device currents natively (`i(v1)`, `@l1[i]`, `@c1[i]`) by passing the
  name through instead of wrapping it in `v(...)`.
- If a signal is unsupported, surface it in the run result and in the CLI
  output (e.g. `warnings: ["i(v1): not captured"]`), even when other signals
  succeed.
- Detect the requested-but-not-`v(...)` form and warn during `simulation-add`.

---

## 5. No in-place update for text schematics (medium impact)

**Observed**

```
$ ice-mcp schematic_create_text --name rlc ...
{ "code": "schematic.name",
  "error": "A schematic named 'rlc' already exists" }
```

To change `R` from 10 to 20 ohms, the schematic had to be deleted and recreated.
Because a simulation referenced it, the simulation also had to be removed first.

**Suggestions**

- Add an upsert/overwrite flag to `schematic_create_text`, or a dedicated
  `schematic_set_text` / `schematic_update` operation.
- When deletion is blocked by dependents, list them and offer a cascade option.
- Allow editing a referenced schematic without deleting the simulation.

---

## 6. `script-run` output is not terminal-friendly (medium impact)

**Observed**

`ice-cli script-run` returns a JSON object that includes every graph's full `x`
and `series` arrays. With ~4,000 samples and three graphs, the output is tens of
thousands of lines. Most of it is graph data that the GUI consumes, not
information a CLI user needs.

**Suggestions**

- Print a compact summary by default: status, stdout/Markdown, and
  `graphs: ["Step response", ...]` with point counts.
- Add `--json` for the full payload and `--quiet` / `--summary` modes.
- Persist graph JSON to the build directory (it already exists) and print the
  path.

---

## 7. Orphan files and unignored caches (low impact)

**Observed**

- `script-delete` removed the metadata but left `scripts/probe.py` on disk
  (documented behavior, but easy to miss).
- Running a script created `scripts/__pycache__/`, while `.gitignore` only
  contains `build/`.
- The repo started with an unregistered `scripts/simulation-script.py` template,
  so `script-list` was empty even though a script file existed.

**Suggestions**

- Add `--purge` to `script-delete` to remove the file.
- Add `__pycache__/` and `*.pyc` to the default `.gitignore`, or run scripts
  with `PYTHONDONTWRITEBYTECODE=1`.
- Move the template out of `scripts/` or register it, so file presence and
  `script-list` agree.

---

## 8. Small usability gaps

- **Graph limit clearing is unclear.** `simulation-graph-limits` takes positional
  optional values; there is no obvious way to remove limits. A `--clear` flag or
  explicit `none` values would help.
- **Auto-created default graph name.** `simulation-add` creates `Graph 1`, which
  then needs renaming. A name option (or naming it after the schematic) would be
  nicer.
- **Text-schematic syntax is undocumented.** Nothing in `AGENTS.md` explains
  which SPICE subset is accepted, how `.end` is handled, or that only transient
  analysis is supported. A short reference with one RLC example would shorten
  discovery time.
- **Error messages are terse.** `schematic-add-text` usage errors do not mention
  that `[pdk]` and `[spice-file]` are optional, and the runner error gives no
  remediation.

---

## What worked well

- Text schematics and netlist echo are clear and predictable.
- The ngspice transient run is fast, deterministic, and stored with a useful
  fingerprint and metadata.
- `icepy_sim` metrics (`overshoot_percent`, `rise_time`, `settling_time`,
  `ringing_frequency`, `damping_ratio`, `quality_factor`) matched hand theory
  closely and made the showcase easy to write.
- The `ctx.graph(...)` API supports both stored traces and derived arrays, which
  made current/energy plots straightforward once the runner was available.
- Project validation returned a clean result, and all mutations were saved.

## Prioritized improvements

1. Install and verify the Python report runner automatically; add a doctor check.
2. Resolve multi-client state (reload, lock, or detect on write).
3. Support current probes and warn about partially missing signals.
4. Add text-schematic update/upsert and guided dependency handling.
5. Add simulation create/run to MCP so a single client can own the workflow.
6. Make `script-run` output summary-first with opt-in full JSON.
7. Polish: purge on delete, ignore `__pycache__`, graph-limit clearing, docs.
