# Training data

Agent-generated analogue and mixed-signal designs, produced with ICE itself.

## Contents

| Path | Purpose |
| --- | --- |
| `circuits.csv` | Multi-table catalogue of analogue and mixed-signal circuits. Each table starts with its own header row, contains a `type` and `name` column, and has type-specific specification columns. |
| `creation_tasks.csv` | One creation task per circuit and target PDK (88 circuits × 3 PDKs = 264 tasks). Each row has a task id, circuit type/name, PDK, specification summary, and a ready-to-use prompt. |
| `generate_creation_tasks.py` | Regenerates `creation_tasks.csv` from `circuits.csv`. |
| `training_agent.py` | Runs selected creation tasks with an OpenRouter model, drives `ice-mcp` (or `ice-cli`), creates an openable ICE project, simulates it, and scores specification closeness and process efficiency. |
| `runs/` | One folder per task run, plus a batch `task_results_<timestamp>.csv` summary. |

`circuits.csv` is a concatenation of CSV tables separated by blank lines. Every
table has its own header row. The `type` column identifies the circuit family,
`name` is the circuit name, and the remaining columns are the specification
parameters for that family. Specification cells are intentionally blank in the
catalogue so runs can fill them with measured values.

## Running

```sh
export OPENROUTER_API_KEY=sk-or-...
python3 training_data/training_agent.py                    # prompts for key, model, count and start task
python3 training_data/training_agent.py --task 12 --count 3
python3 training_data/training_agent.py --task "Wien-bridge oscillator" --count 3
python3 training_data/training_agent.py --all
python3 training_data/training_agent.py --list             # show all creation tasks
python3 training_data/training_agent.py --transport ice-cli
```

When no `--task`, `--count` or `--all` is given, the script prompts for the
OpenRouter key if needed, the model slug, whether to run every task, how many
tasks to run, and the starting task number/id/name. In noninteractive runs use
`--model <slug>`.

By default the script starts `ice-mcp` over stdio and exposes its schematic and
simulation tools to the model. The model can create a text SPICE cell, configure
and run a transient simulation, and inspect the stored result. If it returns the
JSON proposal without calling any tools, the harness creates and simulates that
proposal through `ice-mcp`, so every task exercises the MCP server. Use
`--no-tools` to keep the tools away from the model while still executing through
`ice-mcp`, or `--transport ice-cli` to use the older direct CLI path. Tool
calling requires an OpenRouter model/provider that supports function tools; use
`--no-tools` when a model does not.

`ice-mcp` (or `ice-cli` for the legacy transport) and `ngspice` must be on
`PATH`. Set `ICE_MCP` or `ICE_CLI` to use another binary. The API key is taken
from `--api-key`, else `$OPENROUTER_API_KEY`, else the
`OPENROUTER_API_KEY` line in `training_data/.env`; if none is configured the
script prompts for it. The `.env` file is git-ignored and is the easiest way to
run without exporting anything:

```sh
# training_data/.env
OPENROUTER_API_KEY=sk-or-...
```

The key is never written into a run's transcript or report. Without credits on
the account, paid slugs (including `deepseek/...`) return HTTP 402; free slugs
end in `:free`. `--model` selects any OpenRouter slug, the default is the free
`nvidia/nemotron-3-ultra-550b-a55b:free` (a reasoning model, so its thoughts are
captured in `thoughts.json`), and `OPENROUTER_MODEL` overrides it.

The agent gets `--attempts` design attempts (default 3). Within an attempt it
may use tools for up to `--tool-rounds` model turns (default 12), followed by one
final JSON-only turn. Each attempt is told either the simulator error or the
measured specification table, so it can repair a broken netlist or
misspecification. A run stops as soon as every specification is met.

In MCP mode the tools are filtered to the project, schematic, PDK, and
simulation operations needed for circuit creation; layout, LVS, PEX, and
simulation-script tools are not exposed. Schematic-writing tool calls are
checked for PDK subcircuits used with the `M` designator before a cell or
simulation is created. The harness also checks that a model-run simulation used
the same netlist and target PDK as the returned proposal. If it did not, the
harness runs the returned proposal itself so the score always comes from the
netlist in `proposal.json`. Plain-text tool calls are rejected with a correction
instead of being parsed as JSON proposals.

## What a run produces

```text
runs/<timestamp>_<task_id>/
  thoughts.json           full transcript: prompts, model reasoning, replies,
                          every MCP/ICE tool call, simulator output, final result
  mcp.log                 ice-mcp diagnostics (MCP transport only)
  netlist.sp              the netlist that was simulated
  netlist_attemptN.sp     each attempt (kept for failed/repair turns)
  simulation.json         raw stored ICE run (waveforms, provenance)
  proposal.json           the model's final structured answer
  result.json             task scores and artifact paths
  project/                the ICE project; open project.icepro in the ICE GUI
```

Each batch also writes `runs/task_results_<timestamp>.csv`, with one row per
task containing the specification closeness and process-efficiency scores.

The two scores are:

- **Specification closeness** — a 0-100% continuous distance between the
  measured values and the measurable targets proposed in response to the task.
  The prompt makes the task specifications authoritative and asks the model to
  copy or proxy them without relaxing them.
- **Process efficiency** — 40% successful ICE tool calls (MCP tool calls in the
  default transport), 30% useful answer text versus reasoning text, and 30%
  attempt efficiency from the number of model turns. Failed tasks are
  multiplied by 0.25.

`thoughts.json` includes the measured specification table (target, measured
value, pass/fail status, and closeness score) plus the performance metrics for
every captured signal. `result.json` contains the task scores and artifact
paths.

## Design constraints the agent works under

ICE currently runs ngspice **transient** analysis only. Each task names one of
the supported PDKs (`ihp-sg13g2`, `sky130A`, `gf180mcuA`), and ICE applies that
PDK's model setup automatically. The prompt requires target-PDK devices,
transient-friendly stimuli, and measurable specifications from the metrics the
harness supports. PDK transistors such as `sg13_lv_nmos`,
`sky130_fd_pr__nfet_01v8`, and `nfet_03v3` are subcircuits and must use the `X`
designator, not `M`. Circuits that need AC, DC-sweep, noise or distortion
analysis are out of scope until ICE grows those analyses.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Every selected task produced a successful simulation and report. |
| `1` | One or more selected tasks failed. |
| `2` | Missing API key, missing `ice-mcp`/`ice-cli`, or invalid task selection. |
