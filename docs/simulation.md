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

`runAsync` executes through a worker future and reports progress through a
callback. Callers crossing into a GUI event loop must queue those progress
events onto the application thread; the core adapter never touches Qt objects.
Cancellation is cooperative and backend adapters must check the token between
work units. Experiments enumerate parameter combinations deterministically and
reuse the same runner contract.

CLI example:

```text
ice-cli simulate result.json 1.0 0.01 5000
```

The optional arguments are stop time, step size, and timeout in milliseconds.
