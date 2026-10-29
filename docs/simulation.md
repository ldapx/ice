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
run is persisted so reports, notebooks, and external tools can analyze it
later. By default, stored runs live under the project build directory:

```text
build/simulations/<simulation-id>-<simulation-name>/runs/
```

Each run is a JSON object with `format: "ice-simulation-result"`, run metadata,
the request/netlist, time samples, traces, and graph metadata. Runs are written
atomically and include failed, cancelled, and timed-out terminal states.

The stored format is intentionally new-only; legacy bare result JSON is not
read by the report tooling.

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

## Reports

Project reports are Python files under `reports/` and are stored as
`ProjectReport` metadata in `project.icepro`. A report can query every
simulation and run in the project.

```python
def build_report(ctx):
    runs = ctx.latest_many()
    return "# Report\n\n..."  # Markdown
```

The report runner supplies `ctx.project_root`, `ctx.simulations()`,
`ctx.runs()`, `ctx.latest()`, `ctx.latest_many()`, and `ctx.load()`. Reports are
executed by `assets/simulation/report_runner.py`. The project-level report view
inside the Simulation workspace edits the Python source; runner messages,
errors, and output are appended to the **Messages** tab.

## CLI

```text
ice-cli simulate <project> <schematic> [stop] [step] [timeout-ms] [signals] [output.json]
ice-cli simulate-netlist <netlist-file> [stop] [step] [timeout-ms] [signals] [output.json]
ice-cli report-add <project> <name> [source-file]
ice-cli report-read <project> <report>
ice-cli report-write <project> <report> <source-file|->
ice-cli report-run <project> <report> [timeout-ms]
```

When no output path is supplied, the run is stored under the project build
directory. The optional arguments are stop time, step size, timeout in
milliseconds, and captured signals. The `report-*` commands let agents create,
inspect, edit, and run reports against stored runs without launching the GUI.
