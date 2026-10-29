# Simulation Contract

ICE provides a deterministic transient-analysis slice through
`icelib::SimulationRunner`. `NgspiceSimulator` is the backend used by
tests, CLI smoke tests, and reproducible examples. `NgspiceSimulator` is an
isolated shared-library adapter that accepts a caller-owned netlist and returns
the same normalized `SimulationResult` shape.

Simulation jobs have explicit terminal states: `succeeded`, `failed`,
`cancelled`, and `timed_out`. Results contain schema metadata, simulator
identity, an input fingerprint, time samples, named traces, and an actionable
error string when a job does not succeed.

Simulation properties define the **Signals to capture** list, a comma-separated
set of vectors requested from the simulator (for example `v(out), v(in)`). The
adjacent **Select** button switches to the schematic so a node, pin, or wire can
be picked directly; the resulting `v(node)` signal is appended to the list and
the editor returns to the Simulation tab.

A simulation may point at either a drawn schematic or a text SPICE schematic.
Text source is used directly as the circuit netlist, with analysis-control
directives (`.tran`, `.dc`, `.ac`, `.op`) and `.end` removed because the
simulation properties supply the analysis. For text schematics the **Build
Netlist** button and the schematic signal-picker button are hidden; **Run
Simulation** is available immediately. Drawn schematics keep the build-then-run
flow and the signal picker.

Graphs are presentation-only views over a result. Each graph selects a captured
signal or the `time` variable for its X axis and one or more captured signals
(or `time`) for its Y axis. It can also define optional axis limits; blank
limits auto-scale from the returned data. Axis selections and limits change
only how the plot is drawn and never alter the simulation parameters, netlist,
or data acquisition. Plots draw labelled axis ticks and a light grid by
default; the **Grid** button in the graph header toggles the grid on and off.

`runAsync` executes through a worker future and reports progress through a
callback. Callers crossing into a GUI event loop must queue those progress
events onto the application thread; the core adapter never touches Qt objects.
Cancellation is cooperative and backend adapters must check the token between
work units. Experiments enumerate parameter combinations deterministically and
reuse the same runner contract. Netlists are supplied as element/card lines;
the ngspice adapter adds a title comment when the first line is not already a
comment.

## Stored runs

The GUI plots the in-memory result returned by the simulator. Separately, every
run is persisted so simulation scripts, notebooks, and external tools can analyze it
later. By default, stored runs live under the project build directory:

```text
build/simulations/<simulation-id>-<simulation-name>/runs/
```

Each run is a JSON object with `format: "ice-simulation-result"`, run metadata,
the request/netlist, time samples, traces, and graph metadata. Runs are written
atomically and include failed, cancelled, and timed-out terminal states.

The stored format is intentionally new-only; legacy bare result JSON is not
read by the simulation script tooling.

## Python metrics API

`icepy_sim` is a pure-Python API for loading runs and computing metrics. It is
also exposed as `icepy.sim` when the optional nanobind extension is built.

```python
from icepy import sim

runs = sim.load_latest_many(
    "/path/to/project",
    simulations=["rlc_damped", "rlc_tuned"],
)
for name, run in runs.items():
    metrics = run.trace("v(n1)").metrics()
    print(name, metrics.overshoot_percent, metrics.settling_time)
```

Available metrics include min/max/mean/RMS, peak-to-peak, integral, rise/fall
time, overshoot, peak time, settling time, delay, zero crossings, ringing
frequency/period, damping ratio, and quality factor. Timing and ringing values
are `None` when the waveform does not contain enough evidence for them.

The default step thresholds are 10% and 90%, the steady state is the mean of
the last 10% of the time range, and settling uses a 2% band. Overshoot is
measured against the selected step direction, rise/fall crossings are linearly
interpolated, and damping uses a log-decrement estimate from successive peaks.
`MetricOptions` changes these thresholds and `Trace.crossing_time(level)` exposes
raw threshold crossings.

## Simulation scripts

Simulation scripts are Python files under `scripts/` and are stored as
project-level metadata in `project.icepro`. A script can query every simulation
and run in the project, return Markdown, and add graphs that the GUI renders.

```python
def build_script(ctx):
    run = ctx.latest("RLC")
    if run is not None:
        ctx.graph("RLC response", run=run, signals=["in", "n2"])
    return "# Simulation script\n\n..."  # Markdown
```

The runner supplies `ctx.project_root`, `ctx.simulations()`, `ctx.runs()`,
`ctx.latest()`, `ctx.latest_many()`, and `ctx.load()`. `ctx.graph(...)` accepts
a stored run plus signal names, or explicit `x` and `y` arrays for values
computed by the script. Multiple graphs can be added. Scripts are executed by
the shared Python runner. The project-level script view inside the Simulation
workspace edits the Python source; generated graphs appear in tabs below the
editor, while runner messages, errors, and Markdown output are appended to the
**Messages** tab. Generated graphs are cached under
`build/simulation-scripts/` and reload when the script is selected. The legacy
`build_report` function name is still accepted.

## CLI

```text
ice-cli simulate <project> <schematic> [stop] [step] [timeout-ms] [signals] [output.json]
ice-cli simulate-netlist <netlist-file> [stop] [step] [timeout-ms] [signals] [output.json]
ice-cli simulation-run <project> <simulation> [timeout-ms] [output.json]
ice-cli simulation-graph-add <project> <simulation> <name> [x-axis] [y-signals]
ice-cli simulation-graph-list <project> <simulation>
ice-cli simulation-graph-update <project> <simulation> <graph> [name] [x-axis] [y-signals]
ice-cli simulation-graph-limits <project> <simulation> <graph> [x-min] [x-max] [y-min] [y-max]
ice-cli simulation-graph-delete <project> <simulation> <graph>
ice-cli script-add <project> <name> [source-file]
ice-cli script-read <project> <script>
ice-cli script-write <project> <script> <source-file|->
ice-cli script-run <project> <script> [timeout-ms]
```

When no output path is supplied, the run is stored under the project build
directory. The optional arguments are stop time, step size, timeout in
milliseconds, and captured signals. Signals may be comma- or whitespace-
separated. `simulation-add` creates a default graph from the captured signals;
the `simulation-graph-*` commands let agents configure graph metadata (X/Y signals
and optional limits), and the `script-*` commands let agents create, inspect,
edit, and run simulation scripts against stored runs without launching the GUI.
`script-run` also returns the graph data produced by `ctx.graph(...)`.

`simulation-run` runs a saved simulation definition and stores the run under
that simulation's build directory. When a simulation is selected, the GUI loads
its newest stored run into the graph tabs; if no simulation-specific run exists,
it falls back to the newest run for the simulation's schematic. The GUI also
watches `build/simulations` and refreshes the active plots when new runs appear.
