# ice-cli

`ice-cli` is the headless automation surface over the same `icelib` operations
used by the GUI, Python bindings, and MCP server. Successful commands print JSON
to stdout; diagnostics and logs go to stderr.

## Project

```sh
ice-cli project-create <folder|project.icepro> [name]
ice-cli project-info <folder|project.icepro>
ice-cli project-validate <folder|project.icepro>
```

`project-create` initializes `project.icepro`, `sch/`, `lay/`, `sim/`,
`scripts/`, `build/`, `.gitignore`, and `AGENTS.md`.

## Schematic

```sh
ice-cli schematic-add-cell <project> <name> [pdk]
ice-cli schematic-add-text <project> <name> [pdk] [spice-file]
ice-cli schematic-list <project>
ice-cli schematic-info <project> <cell>
ice-cli schematic-add-symbol <project> <cell> <name> [source-cell]
ice-cli schematic-add-instance <project> <cell> <symbol> <name> <x> <y>
ice-cli schematic-add-net <project> <cell> <name> <pin> <pin> ...
ice-cli schematic-netlist <project> <cell>
```

Cells, symbols, and pins may be referenced by name or id. Pins can also use the
`<symbol>.<pin>` form.

## Layout

```sh
ice-cli layout-add-cell <project> <name> [pdk]
ice-cli layout-add-procedural <project> <name> [pdk] [script-file]
ice-cli layout-create-from-schematic <project> <schematic> [layout-name]
ice-cli layout-list <project>
ice-cli layout-info <project> <cell>
ice-cli layout-add-layer <project> <name> <number>
ice-cli layout-add-rectangle <project> <cell> <layer> <x> <y> <width> <height>
ice-cli layout-add-polygon <project> <cell> <layer> <x,y> <x,y> <x,y> ...
ice-cli layout-add-port <project> <cell> <name> <layer> <x> <y> <width> <height>
ice-cli layout-add-instance <project> <parent> <child> <name> <x> <y> [orientation]
ice-cli layout-export-gds <project> <cell> <output.gds>
```

Coordinates are integer database units.

## Simulation

```sh
ice-cli simulation-add <project> <name> <schematic> [analysis] [stop] [step] [signals]
ice-cli simulation-list <project>
ice-cli simulation-run <project> <simulation> [timeout-ms] [output.json]
ice-cli simulation-graph-add <project> <simulation> <name> [x-axis] [y-signals]
ice-cli simulation-graph-list <project> <simulation>
ice-cli simulation-graph-info <project> <simulation> <graph>
ice-cli simulation-graph-update <project> <simulation> <graph> [name] [x-axis] [y-signals]
ice-cli simulation-graph-limits <project> <simulation> <graph> [x-min] [x-max] [y-min] [y-max]
ice-cli simulation-graph-delete <project> <simulation> <graph>
ice-cli simulate <project> <schematic> [stop] [step] [timeout-ms] [signals] [output.json]
ice-cli simulate-netlist <netlist-file> [stop] [step] [timeout-ms] [signals] [output.json]
```

Time values accept SPICE-style engineering suffixes such as `1u`, `100n`, and
`2m`. Signals may be comma- or whitespace-separated, for example `1,2`,
`out,v(out)`, or `IN N1 N2`. Analysis aliases such as `tran` are stored as
`transient`.

`simulation-run` uses the stored simulation definition and writes its result
under the simulation's build directory, so the GUI can load it as the latest
run. `simulate` runs a schematic directly without a simulation definition.

Without an explicit output path, the simulation writes a stored run under:

```text
build/simulations/<simulation-id>-<name>/runs/<timestamp>-<suffix>.json
```

An explicit output path is still accepted but always uses the stored-run
schema. The result includes the request netlist, simulator provenance, graphs,
and trace data.

Drawn and text SPICE schematics can both be used. For text schematics the
stored SPICE source is the circuit netlist; analysis-control directives
(`.tran`, `.dc`, `.ac`, `.op`) and `.end` are removed because `simulate`
supplies the transient analysis.

Simulation graphs are presentation definitions over captured signals.
`simulation-add` creates a default **Graph 1** using the captured signals, and
`simulation-graph-add` also defaults to `time` on X and the simulation's
captured signals on Y. In `simulation-graph-update`, `-` keeps a field unchanged.
In `simulation-graph-limits`, `-` clears a limit. Deleting the last graph is
rejected.

## Simulation scripts

```sh
ice-cli script-add <project> <name> [source-file]
ice-cli script-list <project>
ice-cli script-info <project> <script>
ice-cli script-read <project> <script>
ice-cli script-write <project> <script> <source-file|->
ice-cli script-run <project> <script> [timeout-ms]
ice-cli script-delete <project> <script>
```

`script-add` creates a new Python file under `scripts/` from the built-in
template, records it in `project.icepro`, and saves the project. An optional
source file replaces the template. Use `-` to read the source from stdin.

`script-write` replaces an existing simulation script's source without changing
project metadata. `script-read` returns the source, and `script-run` returns JSON
with the process exit code, timeout state, stdout, stderr, and generated graph
data. Script runs use the stored simulation runs under `build/simulations/`.
`script-delete` removes only the project association; the Python file remains on
disk. The legacy `report-*` command names remain available as aliases.

The typical agent feedback loop is:

```sh
ice-cli schematic-add-cell project top
ice-cli simulation-add project tran1 top transient 1m 1u 'v(n1)'
ice-cli simulation-graph-add project tran1 "Response" time 'v(n1)'
ice-cli simulation-run project tran1
ice-cli script-add project "RLC Comparison"
ice-cli script-write project "RLC Comparison" script.py
ice-cli script-run project "RLC Comparison"
```
