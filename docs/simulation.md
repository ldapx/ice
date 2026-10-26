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
signal or the `time` variable for its X and Y axes and can define optional axis
limits; blank limits auto-scale from the returned data. Axis selections and
limits change only how the plot is drawn and never alter the simulation
parameters, netlist, or data acquisition. Plots draw labelled axis ticks and a
light grid by default; the **Grid** button in the graph header toggles the grid
on and off.

`runAsync` executes through a worker future and reports progress through a
callback. Callers crossing into a GUI event loop must queue those progress
events onto the application thread; the core adapter never touches Qt objects.
Cancellation is cooperative and backend adapters must check the token between
work units. Experiments enumerate parameter combinations deterministically and
reuse the same runner contract. Netlists are supplied as element/card lines;
the ngspice adapter adds a title comment when the first line is not already a
comment.

CLI example:

```text
ice-cli simulate result.json 1.0 0.01 5000
```

The optional arguments are stop time, step size, and timeout in milliseconds.
