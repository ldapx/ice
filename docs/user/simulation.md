# Simulation

ICE provides analog and digital simulation through `icelib::SimulationRunner`.
The default `AutoSimulator` dispatches to `NgspiceSimulator` for SPICE analyses
and to `IcarusVerilogSimulator` for SystemVerilog digital tests. Both adapters
return the same normalized `SimulationResult` shape.

`NgspiceSimulator` is an isolated shared-library adapter that accepts a
caller-owned netlist. `IcarusVerilogSimulator` writes the SystemVerilog source
to a temporary directory, compiles it with `iverilog -g2012`, runs `vvp`, and
parses the generated Value Change Dump (VCD) into time samples and traces.

Simulation jobs have explicit terminal states: `succeeded`, `failed`,
`cancelled`, and `timed_out`. Results contain schema metadata, simulator
identity, an input fingerprint, x-axis samples, named traces, scalar operating
point/transfer-function results, declarative measurement results, and an
actionable error string when a job does not succeed.

## Analyses

The ngspice adapter supports these analyses:

| Analysis | Result axis | Main parameters |
| --- | --- | --- |
| Transient | time | `start` (optional), `stop`, `step`, `use_initial_conditions` |
| DC sweep | source sweep | `dc_source`, `dc_start`, `dc_stop`, `dc_step` |
| AC sweep | frequency | `ac_variation` (`dec`/`oct`/`lin`), `ac_points`, `ac_start`, `ac_stop` |
| Operating point | operating point | captured signals; results are also stored in `scalars` |
| Noise | frequency | `noise_output`, `noise_input`, `noise_variation`, `noise_points`, `noise_start`, `noise_stop` |
| Transfer function | transfer function | `tf_output`, `tf_source`; scalar results include transfer function and impedances |

All analyses accept `temperature`, `model_section`, `parameters`, and
`source_overrides`. `model_section` rewrites the section argument of `.lib`
lines, parameters become `.param` cards, and source overrides are applied with
ngspice `alter` commands before the analysis runs. AC results include both the
magnitude trace (for example `out`) and a phase trace (`out.phase`, in degrees).
Noise results include `onoise_spectrum`/`inoise_spectrum` and scalar totals.

Simulation properties define the **Signals to capture** list, a comma-separated
set of vectors requested from the simulator (for example `v(out), v(in)`). The
adjacent **Select** button switches to the schematic so a node, pin, or wire can
be picked directly; the resulting `v(node)` signal is appended to the list and
the editor returns to the Simulation tab.

A simulation may point at a drawn schematic, a text SPICE schematic, or a
SystemVerilog source cell. For SPICE schematics the ICE PDK package supplies the
model preamble (`.option`, `.include`, `.lib`) and OSDI initialization. ICE
applies these before the schematic netlist and loads OSDI models explicitly
through libngspice; a global `~/.spiceinit` or `PDK_ROOT`/`PDK` environment is
not used for ICE simulations. Text source is used directly as the circuit
netlist, with analysis-control directives (`.tran`, `.dc`, `.ac`, `.op`,
`.noise`, `.tf`) and `.end` removed because the simulation properties supply the
analysis. For text
schematics and SystemVerilog cells the **Build Netlist** button and the
schematic signal-picker button are hidden; **Run Simulation** is available
immediately. Drawn schematics keep the build-then-run flow and the signal
picker.

## Digital HDL simulation

SystemVerilog cells use the `iverilog` simulator and the `digital` analysis.
When such a cell is selected in the Simulation tab, ICE selects those defaults
automatically. Use **Add SystemVerilog Testbench…** in the Simulation panel
**+** menu to create the testbench source and its simulation together; the
simulation editor then opens a **Testbench** tab for editing that source. The
source is compiled with Icarus Verilog 13 (`-g2012`).

A simulation cell may reference modules defined in other project
SystemVerilog cells. ICE follows module references and concatenates the
selected cell with the source cells it recursively instantiates, so a
`<design>_tb` cell can instantiate a separate `<design>` cell without copying
the design source into the testbench file.

If the source already contains `$dumpfile` and `$dumpvars`, ICE runs its inferred
top module directly. Otherwise ICE generates a wrapper top module that
instantiates the inferred design top, dumps all signals to a temporary VCD, and
compiles with `ice_vcd_top` as the top. A testbench named `<name>_tb` is the
usual source cell. The wrapper allows a plain design block to be simulated too.

The VCD parser converts dump times to seconds using the file's `$timescale`.
Values are replayed onto every timestamp in the dump. Scalar `x`/`z` values and
binary `x`/`z` bits are treated as `0` so digital traces remain graphable; the
run reports parser warnings when a value or requested signal cannot be used.
Captured signal names match either a full VCD hierarchy path or a unique suffix
such as `clk`. When no capture list is set, all VCD traces are returned.

## Mixed-signal simulation

A drawn schematic that instantiates SystemVerilog blocks is simulated with the
`mixed` simulator. ICE parses each block's ports, generates a symbol with
matching pins, and records the analog net attached to every input and output.
Use `schematic_add_hdl_instance` (MCP) or `schematic-add-hdl-instance` (CLI) to
create the instance, then wire it like any other symbol.

`MixedSignalSimulator` alternates between a generated Icarus Verilog testbench
and ngspice:

1. Digital inputs are initialized to zero.
2. The generated testbench reads one bit per input per time step, evaluates all
   HDL blocks, and dumps their outputs to VCD.
3. Digital outputs drive PWL voltage sources in the analog deck. Analog nodes
   connected to HDL inputs are probed, with a 1 GΩ bleed resistor so floating
   input nets still have a DC path.
4. Sampled analog input bits are compared with the previous pass. The loop
   stops when they match, or reports a non-convergence warning after the
   configured iteration count.
5. Analog and digital traces are resampled onto the shared time grid and
   returned together. Digital traces are named `<instance>.<port>`.

The mixed-signal interface supports HDL instances anywhere in the drawn
hierarchy; nested drawn blocks are flattened and use scoped names such as
`U1_U2`. It rejects `inout` ports, unflattened buses, and multiple digital
drivers on one analog node. Scalar logic ports use threshold sampling and PWL
0/1 outputs.
`real` input/output ports are bridged directly: analog values are written to the
digital input file and real VCD outputs drive the analog PWL source. Buses can be
flattened with `bus_mode="flatten"`; pins become `data[0]`, `data[1]`, …
(assumed LSB-first, matching `[width-1:0]`) and the generated testbench
concatenates them. Use `schematic_validate_hierarchy` /
`schematic-validate-hierarchy` to inspect those conditions before running. A
pure SPICE netlist of a mixed schematic is rejected with an explicit error
instead of silently omitting the HDL block. Mixed simulations created without
explicit timing default to a 1 µs stop time and a 1 ns step so a typical digital
waveform is not simulated over an unnecessarily long interval.

`simulation_trace` returns decimated samples for named analog or digital
signals. `simulation_run` also reports `analog_signals`, `digital_signals`,
applied PDK setup, graph definitions, hints, and suggested follow-up calls.

After a successful run, ICE fills an empty **Signals to capture** list and
empty graph Y selections from the first traces. This happens in the GUI, the
CLI, and MCP so a first `simulation-run` produces a usable stored graph. The
graph toolbar **Load VCD** action performs the same population from an external
VCD file without running a simulator.

A drawn schematic that instances another drawn schematic emits a hierarchical
SPICE netlist. Each referenced schematic appears once as a `.subckt`, with its
Input and Output instances defining the port order, and the parent instance is
emitted as `X<name> <connected nodes...> <subckt name>`. Nested subcircuits are
emitted recursively before the parent body.

Graphs are presentation-only views over a result. Each graph selects a captured
signal or the result's primary x axis (time, frequency, source sweep, or
operating point) for its X axis and one or more captured signals for its Y axis.
It can also define optional axis limits; blank limits auto-scale from the
returned data. Axis selections and limits change only how the plot is drawn and
never alter the simulation parameters, netlist, or data acquisition. Plots draw
labelled axis ticks and a light grid by default; the **Grid** button in the
graph header toggles the grid on and off.

Graphs also store waveform-analysis settings:

| Setting | Meaning |
| --- | --- |
| `view` | `linear`, `db` (20·log10 magnitude), or `phase` |
| `fft` | Frequency-domain view of the selected traces (`window`, `points`) |
| `derived_traces` | Named expressions such as `out - in`, `db(out)`, `derivative(i(v1))` |
| `overlays` | Stored run ids drawn on the same graph with variant labels |
| `markers` | Persistent point annotations with a signal, X, and optional Y |
| `show_measurements` | Draw declarative measurement values inside the frame |

Expressions support `+ - * /`, parentheses, and `abs`, `sqrt`, `exp`, `ln`,
`log10`, `db`, `mag`, `phase`, `derivative`, `integral`, `min`, and `max`.
Trace lookup is case-insensitive, ignores whitespace, and accepts a bare node or
`v(node)`. FFT analysis resamples non-uniform transient output onto a uniform
grid, applies a Hann, Hamming, Blackman, or rectangular window, and reports
amplitude (and phase) versus frequency. FFT phase is referenced to a cosine, so
a pure sine with phase φ appears at `φ - 90°`. View and derived traces are
display only, so stored run files remain untouched.

Interactive plot controls are described in the GUI documentation: wheel zoom,
pan, box zoom, cursors A/B with delta readout, marker placement, and measurement
annotations. These settings persist in the project and are available to the CLI
and MCP through `simulation-graph-update`, `simulation-graph-derive`,
`simulation-graph-overlay`, and `simulation-graph-marker-add`.


`runAsync` executes through a worker future and reports progress through a
callback. Callers crossing into a GUI event loop must queue those progress
events onto the application thread; the core adapter never touches Qt objects.
Cancellation is cooperative and backend adapters must check the token between
work units. Experiments enumerate parameter combinations deterministically and
reuse the same runner contract. Netlists are supplied as element/card lines;
the ngspice adapter adds a title comment when the first line is not already a
comment.

## Corners, sweeps, and Monte Carlo

A simulation can run as an experiment across process/temperature/supply
corners, parameter sweeps, and Monte Carlo samples:

- **Corners** set a model section, temperature, source overrides, and `.param`
  values. `simulation-corner-defaults` populates the TT/FF/SS-style sections
  declared by the active PDK; custom corners can be added explicitly.
- **Sweeps** expand one or more parameter axes as a Cartesian product. The
  names `temperature`, `amplitude`, and `time_constant` map to the analysis
  fields; `supply` or `source.<name>` sets a source override. Other names become
  `.param` values used by the netlist.
- **Monte Carlo** samples temperature, a nominated supply source, and any
  parameter percentages with a deterministic seed.

Each result point records its corner, sample index, applied parameters, x-axis,
traces, scalars, and measurement results. In the GUI, open **Experiment
Setup…** next to **Run Simulation** to edit the analysis parameters, corners,
sweeps, Monte Carlo settings, and measurements, then run the experiment from the
same dialog. The **Results** tab lists one row per variant.

From the CLI:

```sh
ice-cli simulation-configure <project> <simulation> --analysis ac \
  --ac-points 20 --ac-start 1 --ac-stop 1e8 --signals out,in
ice-cli simulation-corner-defaults <project> <simulation>
ice-cli simulation-corner-add <project> <simulation> ff --section ff \
  --temperature 85 --supply V1=1.1
ice-cli simulation-sweep-add <project> <simulation> temperature 27 85
ice-cli simulation-monte-carlo <project> <simulation> --samples 50 \
  --temperature-sigma 5 --supply-sigma-percent 5 --supply-nominal 1.8
ice-cli simulation-experiment <project> <simulation>
```

MCP exposes the same configuration through `simulation_update` and runs it
with `simulation_experiment`; `simulation_corner_defaults` loads the PDK
sections. Every point is stored under `build/simulations` with a variant label
unless `store=false`/`--no-store` is requested.

## Declarative measurements

A simulation can declare metrics that are evaluated automatically after every
run:

| Metric | Required traces | Unit |
| --- | --- | --- |
| `gain` | AC output and input magnitude (input optional) | dB for AC, V/V for transient |
| `bandwidth` | AC output/input magnitude | Hz |
| `phase_margin` | AC output/input magnitude and `.phase` traces | degrees |
| `slew_rate` | transient signal | V/s |
| `settling_time` | transient signal | s |
| `power` | transient current/power signal; set `voltage` for supply current | W |

Add a measurement from the GUI experiment dialog, with
`simulation-measure-add`, or through `simulation_update`'s `measurements` array.
Results are stored on the run and returned by `simulation_run`/`simulation_result`:

```sh
ice-cli simulation-measure-add <project> <simulation> gain gain \
  --signal out --input in
ice-cli simulation-measure-add <project> <simulation> settling settling_time \
  --signal out --tolerance 0.02
ice-cli simulation-measure <project> <simulation>
```

For a one-off check without changing the simulation:

```sh
ice-cli simulation-measure <project> <simulation> --metric slew_rate --signal out
```

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

`simulation_result`/`simulation_signals` include per-trace min/max/amplitude,
mean, RMS, first/last values, and point counts. `simulation_measure` (MCP) and
`ice-cli simulation-measure` add zero crossings, rising crossings, period,
frequency, and settling time. These are the lightweight checks to use before
claiming that an oscillator or transient has met its specification.

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
`build/simulation-scripts/` and reload when the script is selected. Simulation
scripts must define `build_script(ctx)`.

## CLI

```text
ice-cli simulate <project> <schematic> [stop] [step] [timeout-ms] [signals] [output.json]
ice-cli simulate-netlist <netlist-file> [stop] [step] [timeout-ms] [signals] [output.json]
ice-cli simulation-add <project> <name> <schematic> [analysis] [stop] [step] [signals...]
ice-cli simulation-configure <project> <simulation> [analysis options]
ice-cli simulation-run <project> <simulation> [timeout-ms] [output.json]
ice-cli simulation-experiment <project> <simulation> [timeout-ms] [--no-store] [--output file]
ice-cli simulation-remove <project> <simulation>
ice-cli simulation-corner-defaults <project> <simulation>
ice-cli simulation-corner-add <project> <simulation> <name> [--section s] [--temperature c] [--supply n=v]...
ice-cli simulation-corner-list <project> <simulation>
ice-cli simulation-corner-remove <project> <simulation> <name>
ice-cli simulation-sweep-add <project> <simulation> <parameter> <value...>
ice-cli simulation-sweep-list <project> <simulation>
ice-cli simulation-sweep-remove <project> <simulation> <parameter>
ice-cli simulation-monte-carlo <project> <simulation> [--samples n] [--temperature-sigma c] ...
ice-cli simulation-measure-add <project> <simulation> <name> <metric> [--signal s] [--input s] ...
ice-cli simulation-measure-list <project> <simulation>
ice-cli simulation-measure-remove <project> <simulation> <name>
ice-cli simulation-trace <project> <simulation> [signals] [max-points]
ice-cli simulation-measure <project> <simulation> [signal] [threshold] \
  [--metric gain|bandwidth|phase_margin|slew_rate|settling_time|power] \
  [--signal s] [--input s] [--voltage v] [--tolerance v]
ice-cli simulation-graph-add <project> <simulation> <name> [x-axis] [y-signals]
ice-cli simulation-graph-list <project> <simulation>
ice-cli simulation-graph-update <project> <simulation> <graph> [name] [x-axis] [y-signals] \
  [--view linear|db|phase] [--fft on|off] [--fft-window w] [--fft-points n] \
  [--derived name=expr] [--overlay run-id[=label]]
ice-cli simulation-graph-derive <project> <simulation> <graph> <name> <expression>
ice-cli simulation-graph-overlay <project> <simulation> <graph> <run-id> [label]
ice-cli simulation-graph-marker-add <project> <simulation> <graph> <label> <x> [--y v] [--signal s]
ice-cli simulation-runs <project> [simulation]
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

Text schematics accept ordinary ngspice element cards and comments. ICE owns the
analysis directive, so `.tran`, `.dc`, `.ac`, `.op`, `.noise`, `.tf`, and `.end`
are ignored from the text source; the current simulation properties provide the
analysis. Capture node voltages as `v(node)` (or just `node`) and device vectors
as `i(v1)` or `@l1[i]`. Unsupported requested vectors are reported in the run's
`warnings` array instead of being silently discarded.

SystemVerilog source is compiled and run in the project directory when one is
known, so relative `` `include `` paths resolve against the project root. The
simulator has a 10 second `vvp` timeout; if a testbench does not call `$finish`,
ICE keeps the partial VCD produced before termination and reports a timeout
warning. `simulation-add` on a SystemVerilog cell defaults to the `iverilog`
simulator and `digital` analysis, so the same `simulation-run` workflow used for
SPICE works without extra flags.
