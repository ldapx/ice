# ice-cli

`ice-cli` is the headless automation surface over the same `icelib` operations
used by the GUI, Python bindings, and MCP server. Successful commands print JSON
to stdout; diagnostics and logs go to stderr.

## Project

```sh
ice-cli project-create <folder|project.icepro> [name]
ice-cli project-info [folder|project.icepro]
ice-cli project-validate <folder|project.icepro>
ice-cli project-repair <folder|project.icepro>
ice-cli project-migrate <folder|project.icepro>
ice-cli doctor [project] [--probe]
ice-cli pdk-info [pdk] [--probe]
```

`project-create` initializes `project.icepro`, `sch/`, `lay/`, `sim/`,
`scripts/`, `build/`, `.gitignore`, and `AGENTS.md`. `project-migrate` refreshes
the versioned generated section of `AGENTS.md` and preserves user text outside
the markers. `project-info` can omit the path: ICE uses `ICE_PROJECT` or searches
the current directory and its parents for a single `project.icepro`.

`ice-cli --version` prints the build fingerprint as JSON, including the git
commit, build time, executable path, and feature flags.

Project metadata and sidecar writes are atomic and guarded by a project lock
and revision check. A CLI process that loaded an older revision fails with
`project.changed` instead of overwriting a newer save from another process.

`doctor` reports Python, ngspice, installed PDKs, simulation model files, and
Liberty timing libraries. `pdks_ready` means at least one installed PDK is
ready; `all_pdks_ready` means every installed PDK is ready. Passing a project
makes `doctor` fail when a PDK used by the project is not simulation-ready.
`--probe` additionally runs a minimal transistor load check.
`pdk-info` returns the recommended `.lib` line, sections, OSDI and
package/OpenVAF status, plus implementation readiness (`implementation_ready`,
`implementation_engine`, `implementation_stack`, `implementation_missing`);
its `--probe` flag runs the same simulation check. `doctor` exits with status `1`
when `ok` is false, so scripts and agents can fail fast on a broken PDK setup.

## Schematic

```sh
ice-cli schematic-add-cell <project> <name> [pdk]
ice-cli schematic-add-text <project> <name> [pdk] [spice-file]
ice-cli schematic-add-text <project> <name> --pdk <pdk> --file <spice-file>
ice-cli schematic-add-sv <project> <name> [pdk] [source-file]
ice-cli schematic-list <project>
ice-cli schematic-info <project> <cell>
ice-cli schematic-remove <project> <cell> [--dry-run] [--cascade]
ice-cli schematic-add-symbol <project> <cell> <name> [source-cell] [--blank]
ice-cli schematic-add-pin <project> <cell> <symbol> <name> [x] [y] [direction]
ice-cli schematic-add-instance <project> <cell> <symbol> <name> <x> <y>
ice-cli schematic-add-component <project> <cell> <type> [name] [x] [y] [param=value ...]
ice-cli schematic-list-symbols [pdk] [query]
ice-cli schematic-add-circuit <project> <cell> <json-file|->
ice-cli schematic-set-parameter <project> <cell> <instance> <name> <value>
ice-cli schematic-add-net <project> <cell> <name> <pin> <pin> ...
ice-cli schematic-validate <project> <cell>
ice-cli schematic-import-spice <project> <name> [--pdk <pdk>] [--mode text|drawn|auto] <file|->
ice-cli schematic-hdl-ports <project> <cell>
ice-cli schematic-add-hdl-instance <project> <cell> <source-cell> <name> <x> <y> [--flatten-buses]
ice-cli schematic-validate-hierarchy <project> <cell>
ice-cli schematic-netlist <project> <cell>
```

Cells, symbols, pins, and instances may be referenced by name or id. Pins can
use the `<instance>.<pin>` or `<symbol>.<pin>` form. `schematic-add-pin` adds a
pin to an existing symbol; direction accepts `left`, `right`, `up`, or `down`.
`schematic-set-parameter` creates or updates an instance parameter such as
`resistance`, `capacitance`, or a source `value`. `schematic-add-symbol` loads
real artwork and pins for known library names (`RES`, `NMOS`, `VDD`, `GND`,
`IDC`, ...). Use `--blank` only for a custom symbol whose pins you will add with
`schematic-add-pin`.

`schematic-add-component` creates a primitive with a generated symbol and
returns its pin ids. `schematic-list-symbols` lists primitive and PDK symbols.
`schematic-add-circuit` takes a JSON object such as
`{"components":[{"type":"resistor","name":"R1","parameters":{"resistance":"1k"}}],
"nets":[{"name":"out","pins":["R1.1","C1.1"]}]}`
and commits the whole circuit atomically. `schematic-validate` checks
connectivity, component values, PDK setup, and mixed-signal hierarchy.

`schematic-import-spice` creates a text SPICE cell, or a drawn R/C/L/V/I/D
schematic with `--mode drawn`. `--mode auto` chooses drawn only when the source
is a flat supported netlist. `schematic-remove --dry-run` lists dependencies;
`--cascade` removes dependent simulations and hierarchical symbols too.

`schematic-add-text` accepts either the positional `[pdk] [spice-file]` form or
the unambiguous `--pdk <pdk> --file <spice-file>` form. If a file path is passed
where the PDK is expected, the command accepts it as a file but warns that the
cell was stored with the generic PDK; pass `--pdk` to preserve the intended PDK.
The MCP equivalent is `schematic_create` with `kind: "text"` or
`schematic_create_text`.

`schematic-add-sv` creates a SystemVerilog source cell. The optional file is stored as its source; without a
file, ICE inserts a small digital block template. `schematic-info` and
`schematic-list` report the `systemverilog` flag for these cells.

`schematic-hdl-ports` parses a SystemVerilog cell and returns its module name,
port directions, and widths. `schematic-add-hdl-instance` generates a symbol
whose pins match those ports and places it in a drawn schematic; use
`schematic-add-net` with names such as `U1.in` and `U1.out` to wire it.
`schematic-validate-hierarchy` checks the resulting hierarchy, port mapping,
nets, buses, and mixed-signal support before simulation.

A pure SPICE netlist rejects hierarchical SystemVerilog blocks with an explicit
error instead of silently skipping them. Use mixed-signal simulation for drawn
schematics that contain HDL instances.

## Layout

Layouts are bound to one PDK and one stack variant. Pass the stack as the last
argument when the package declares `[[pdk.stack]]` entries; omitting it selects
the package's `default_stack`. `layout-list` and `layout-info` include the
resolved `stack` field, and `layout-info` also reports it for procedural
layouts. A layout instance whose child uses a different PDK or stack is
rejected.

```sh
ice-cli layout-add-cell <project> <name> [pdk] [stack]
ice-cli layout-add-procedural <project> <name> [pdk] [script-file] [stack]
ice-cli layout-create-from-schematic <project> <schematic> [layout-name]
ice-cli layout-list <project>
ice-cli layout-info <project> <cell|procedural>
ice-cli layout-remove <project> <cell|procedural>
ice-cli layout-add-layer <project> <name> <number> [datatype]
ice-cli layout-add-rectangle <project> <cell> <layer> <x> <y> <width> <height>
ice-cli layout-add-text <project> <cell> <layer> <x> <y> <text> [height]
ice-cli layout-add-polygon <project> <cell> <layer> <x,y> <x,y> <x,y> ...
ice-cli layout-add-port <project> <cell> <name> <layer> <x> <y> <width> <height> [pin options]
ice-cli layout-list-ports <project> <cell>
ice-cli layout-update-port <project> <cell> <port> [pin options]
ice-cli layout-remove-port <project> <cell> <port>
ice-cli layout-add-net <project> <cell> <name>
ice-cli layout-assign-port-net <project> <cell> <port> <net|none>
ice-cli layout-add-instance <project> <parent> <child> <name> <x> <y> [orientation]
ice-cli layout-connectivity <project> <cell>
ice-cli layout-export-gds <project> <cell> <output.gds> [--flatten]
ice-cli layout-export-lef <project> <cell> <output.lef>
ice-cli layout-export-def <project> <cell> <output.def>
ice-cli layout-import-gds <input.gds> <project> [pin-label-layer]
ice-cli layout-import-lef <input.lef> <project>
ice-cli layout-import-def <input.def> <project>
```

Pin options are `--direction input|output|inout|power|ground`, `--side
left|right|top|bottom`, `--net <name-or-id>`, `--label <text>`, `--hidden`,
`--locked`, and for updates `--unlocked`, `--visible`, `--name`, `--x`, `--y`,
`--width`, and `--height`. Coordinates are integer database units.

`layout-remove` accepts either a static layout cell or a procedural layout;
removing a procedural layout also removes its generated static child. Removal
is blocked when instances reference the layout, and library cells cannot be
removed. `layout-export-gds` writes each visible pin as a boundary plus a GDSII
TEXT label. `layout-connectivity` merges objects assigned to the same layout
net and geometry that overlaps or touches on the same layer; via and contact
layers also connect overlapping layers. `layout-export-lef` and
`layout-export-def` write minimal pin metadata for place-and-route exchange.
`layout-import-gds` creates a new project and, when a pin-label layer is given,
converts matching TEXT records into pins. `layout-import-lef` and
`layout-import-def` create a new project from a macro or design's pins and
nets.

## Implementation runs

`implementation-*` commands manage RTL-to-GDS runs. A run stores its configured
top module, PDK, stack, RTL sources, constraints, and physical policy in
`impl/*.iceimpl`; generated configuration and stage scripts are written under
`build/impl/<run>/`.

```sh
ice-cli implementation-create <project> <name> --top <module> --pdk <pdk> \
  [--stack <stack>] [--source <file>]... [--source-cell <cell>]... \
  [--sdc <file>] [--std-cell-lib <name>] [--corner <corner>] \
  [--utilization <value>] [--aspect-ratio <value>] \
  [--placement-density <value>] [--threads <n>] [--backend <name>] \
  [--die-area x,y,w,h] [--core-area x,y,w,h] [--site <name>] \
  [--io-horizontal-layers <a,b>] [--io-vertical-layers <a,b>] \
  [--macro cell,x,y[,orientation]]... \
  [--timing-driven] [--routability-driven] [--no-cts] \
  [--cts-root-buffer <cell>] [--cts-buffers <a,b>] \
  [--routing-min-layer <layer>] [--routing-max-layer <layer>] [--no-antenna-repair] \
  [--seed <n>] [--timeout <seconds>]
ice-cli implementation-update <project> <run> [--from <stage>] [implementation options]
ice-cli implementation-list <project>
ice-cli implementation-status <project> <run>
ice-cli implementation-prepare <project> <run>
ice-cli implementation-run <project> <run> [--from <stage>] [--to <stage>] [--timeout <seconds>] [--verbose]
ice-cli implementation-map <project> <run> [--output <path>] [--verbose]
ice-cli implementation-log <project> <run> [--stage <stage>] [--tail <lines>]
ice-cli implementation-cancel <project> <run>
ice-cli implementation-import <project> <run> [--layout-name <name>] [--replace]
ice-cli implementation-export-gds <project> <run> [--output <path>]
ice-cli implementation-toolchain
ice-cli implementation-delete <project> <run> [--purge]
```

`implementation-create` resolves the package default stack when `--stack` is
omitted. `--source-cell` refers to an existing SystemVerilog source cell in the
project; `--source` refers to an external Verilog/SystemVerilog file.
`--utilization` is a fraction of the core area (`0.5` means 50%).
`--die-area` and `--core-area` take `x,y,width,height` in database units;
`--macro` may be repeated. `implementation-update` edits an existing run and
invalidates `--from` (default `elaboration`) and every later stage. Run
`implementation-prepare` to inspect the resolved platform, copied RTL, and
generated scripts without executing an external tool. The preparation
generates:

- `scripts/00-synth.ys` for Yosys synthesis;
- `scripts/01-techmap.ys` for technology mapping (`implementation-map`);
- `scripts/01-floorplan.tcl`;
- `scripts/02-place.tcl`;
- `scripts/03-cts.tcl`;
- `scripts/04-route.tcl`;
- `scripts/05-final.tcl`.

`implementation-run` executes those scripts through the cancellable process
runner. When the C++ backend is available (`ice-openroad` next to `ice-cli` or
set through `ICE_OPENROAD_HELPER`), the OpenROAD physical stages use the
OpenROAD C++ APIs instead of generated Tcl. Set `ICE_OPENROAD_BACKEND=tcl` to
force the legacy backend or `ICE_OPENROAD_BACKEND=cpp` to require the C++
backend. The same helper provides read-only ODB tooling (`ice-openroad
odb-info`, `ice-openroad odb-preview`) and native heat-map CSV export
(`ice-openroad heatmap --kind placement|routing|power|pin|rudy|irdrop`). Build
it from the pinned OpenROAD/OpenSTA revisions with `xmake openroad-sdk`; use
`scripts/build-ice-openroad.sh --sdk-install <prefix>` for an SDK install. See
`openroad_sdk/README.md`. Yosys synthesis is technology-independent and writes
the generic `synth/1_synth.v` netlist. `implementation-map` then maps it to the platform
standard cells with `dfflibmap`/`abc`, writes `synth/2_tech_map.v`, records
`logs/01-techmap.log` and `scripts/01-techmap.ys`, and copies the result to
`--output` when provided. Floorplan consumes `synth/2_tech_map.v`; if the mapped
netlist is missing when the floorplan stage starts, `implementation-run` maps it
automatically after a successful synthesis, so a full run does not require a
separate manual export. Re-running synthesis deletes the
stale mapped netlist. When Graphviz (`dot`) is available, Yosys also writes
`reports/elaboration.svg` and `reports/synthesis.svg`. Each stage records
start/finish timestamps, status, merged log, artifact paths, and OpenROAD
`-metrics` JSON. Yosys statistics are stored as
`synth.cells`, `synth.wires`, `synth.chip_area_um2`, and related keys. A failed stage
keeps the preceding ODB checkpoint intact, and rerunning from a stage resets
only that stage and the stages after it.

After `final`, ICE attempts to stream `def/6_final.def` plus the platform
standard-cell GDS decks to `final/6_final.gds` with KLayout. Package stacks can
declare a merged `gds` file; OpenROAD-flow-scripts platform fallbacks discover
`gds/*.gds` automatically. If KLayout or standard-cell GDS decks are missing,
the final stage still succeeds and records a warning. Use
`implementation-export-gds` to rerun or redirect the export explicitly.

`--from` defaults to `synthesis`, `--to` defaults to `final`, and `--timeout`
sets the per-stage timeout. HDL runs may start at `elaboration`; when Graphviz
is available that stage writes `reports/elaboration.svg`. Ctrl-C cancels the
child process group and records the stage as cancelled. From another terminal, `implementation-cancel` reads the
PID recorded under `build/impl/<run>/run.pid` and sends SIGTERM after checking
that the process is OpenROAD or Yosys.

`implementation-log` prints the bounded merged log for one stage; `--tail` keeps
only the last N lines. `implementation-import` reads the newest available DEF
checkpoint (`6_final.def` down to `2_floorplan.def`), creates package-mapped
layers, top-level pins and nets, standard-cell instances, and routed paths, and
stores the new layout id on the run. `--replace` reuses the previously imported
layout instead of creating another cell.

`implementation-toolchain` reports the resolved `openroad`, `yosys`, and
`klayout` executables. Set `ICE_OPENROAD`, `ICE_YOSYS`, or `ICE_KLAYOUT` to
override discovery.

When a package declares `[pdk.implementation.orfs]`, ICE resolves missing LEF,
Liberty, site, IO-layer, routing, and OpenRCX rule defaults from
`OPENROAD_FLOW_SCRIPTS`/`ICE_OPENROAD_FLOW_SCRIPTS`. The helper falls back to
`$HOME/repositories/OpenROAD-flow-scripts` for local development.

## Verification

```sh
ice-cli lvs <project> <schematic> <layout>
ice-cli lvs-auto <project> <layout>
ice-cli pex <project> <layout> [schematic-name]
ice-cli drc <project> <layout> [--no-flatten] [--output <dir>] [--suite <name>] \
  [--top-cell <name>] [--timeout <seconds>]
```

`project-repair` loads a project with recovery enabled: malformed sidecar files
are moved under `build/recovery/`, the valid cells are kept, and the project is
saved again. Use it when a hand-edited or partially written `sch/`, `lay/`,
`impl/`, or `sim/` file prevents normal loading. Strict load errors now include the exact
repair/recovery hint instead of requiring the user to guess.

`ice-mcp --project <path> --recover` provides the same recovery at MCP startup.
Without `--recover`, a malformed startup project leaves the server running in a
degraded state so `project_repair` and `project_open(recover=true)` remain
callable.

`lvs` compares a schematic against a layout and prints the LVS report as JSON.
`lvs-auto` infers the schematic from planned-device links or matching names.
Both commands exit with status `0` for a match and `1` when the report contains
errors or warnings. The comparison covers device presence, reference names,
device types, shared device parameters, net topology from planned connections,
and top-level pins matched by name, direction, and bound net. The JSON report
includes `ports_matched` and the `missing-port`, `extra-port`, `port-direction`,
and `port-net` mismatch categories.

`drc` exports the layout to a temporary GDS, runs `gdscheck` with the process
deck derived from the layout PDK, and prints normalized violations. The work
directory defaults to `build/drc/<layout>/` and contains `layout.gds`,
`drc.lyrdb`, and `drc.log`. The command exits with status `1` when the report
cannot be produced or contains error-severity violations; warnings alone keep
status `0`.

`pex` extracts placed device-library instances, PDK-rule polygon devices, and
first-order per-net resistance and capacitance from layout geometry, then
creates or updates a text SPICE schematic in the project hierarchy. Polygon
device rules come from the installed package's `layout/extraction.json`. When no
layout devices are recognized it falls back to the inferred source schematic
device netlist. The report JSON
includes the layout and schematic ids, per-net area, perimeter, resistance,
capacitance, extracted devices, and the complete generated SPICE source. An
optional third argument overrides the generated cell name.

## PDK setup

`ice-cli doctor [project]` reports installed PDK packages and whether each one
is simulation-ready. `ice-cli pdk-info <pdk> --probe` shows the exact package
model directory, section names, OSDI/OpenVAF status, recommended setup lines,
example device names, and runs a minimal transistor load check.

Simulation PDKs come from ICE packages only. Volare, CIEL, `PDK_ROOT`, `PDK`,
and global `~/.spiceinit` files are not used. Install a PDK package with
`ice-pkg`; if it uses Verilog-A models, build the OSDI cache once:

```sh
ice-cli pdk-build ihp-sg13g2
```

PDKs use different ngspice layouts: IHP has `models/cornerMOSlv.lib`, Sky130 has
`sky130.lib.spice` at the ngspice root, and GF180 has `.ngspice` files at the
root. The probe handles all three:

- Sky130: `.option scale=1u` and `.lib ".../sky130.lib.spice" tt`; devices such
  as `sky130_fd_pr__nfet_01v8`.
- GF180: `.include ".../design.ngspice"` then
  `.lib ".../sm141064.ngspice" typical`; devices such as `nfet_03v3`.
- IHP SG13G2: `.lib ".../cornerMOSlv.lib" mos_tt`; devices such as
  `sg13_lv_nmos`. Do not `.include` `cornerMOSlv.lib`; its internal `.LIB`
  sections must be selected by a `.lib` call. PSP devices also need the
  package's OSDI models; `pdk-build` compiles them and ICE loads them
  explicitly for each simulation.

If `doctor` reports `pdk_ready: false`, install or build the ICE PDK package
before trying to simulate a transistor-level circuit.

## Simulation

```sh
ice-cli simulation-add <project> <name> <schematic> [analysis] [stop] [step] [signals]
ice-cli simulation-list <project>
ice-cli simulation-remove <project> <simulation>
ice-cli simulation-run <project> <simulation> [timeout-ms] [output.json]
ice-cli simulation-trace <project> <simulation> [signals] [max-points]
ice-cli simulation-measure <project> <simulation> [signal] [threshold]
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

For `simulation-add`, signals may be comma-separated or passed as multiple
trailing arguments; both forms are stored as a comma-separated capture list.

`simulation-run` uses the stored simulation definition and writes its result
under the simulation's build directory, so the GUI can load it as the latest
run. When the run succeeds, empty capture-signal and graph Y selections are
filled from the returned traces and the project is saved. `simulate` runs a
schematic directly without a simulation definition. `simulation-remove` deletes
the simulation and its graph definitions; stored build runs remain on disk.

`simulation-measure` reads the latest stored run and reports min, max,
amplitude, mean, RMS, first/last values, zero and rising crossings, period,
frequency, and settling time. Use it to validate oscillator startup and
frequency instead of treating `state: succeeded` as proof that the circuit
oscillates. A `threshold` overrides the default midpoint crossing level.

Without an explicit output path, the simulation writes a stored run under:

```text
build/simulations/<simulation-id>-<name>/runs/<timestamp>-<suffix>.json
```

An explicit output path is still accepted but always uses the stored-run
schema. The result includes the request netlist, simulator provenance, graphs,
and trace data.

Drawn SPICE, text SPICE, and SystemVerilog schematics can all be used. For text
schematics the stored SPICE source is the circuit netlist; analysis-control
directives (`.tran`, `.dc`, `.ac`, `.op`) and `.end` are removed because
`simulate` supplies the transient analysis.

`simulation-add` on a SystemVerilog cell stores the `iverilog` simulator and
`digital` analysis automatically. `simulation-run` compiles the source with
`iverilog`, runs `vvp`, and returns VCD-derived traces. The selected cell may
instantiate modules defined in other project SystemVerilog cells; ICE follows
those references and compiles the combined source. `digital` is accepted as
an analysis alias when creating a simulation explicitly. Direct `simulate` on a
SystemVerilog cell, and `simulate-netlist` on a `.sv` file, use the same backend.
If the source has no `$dumpfile`/`$dumpvars`, ICE adds a wrapper module that
dumps the design; a testbench that never calls `$finish` is stopped after 10
seconds and the partial VCD is kept.

A drawn schematic that instantiates SystemVerilog blocks is a mixed-signal
schematic. `simulation-add` automatically stores the `mixed` simulator and
`transient` analysis for it, with 1 µs stop and 1 ns step defaults when no explicit
times are given. `simulation-run` then iterates between ngspice and Icarus
Verilog: digital outputs drive PWL sources in the analog deck, analog input
nodes are sampled through a logic threshold, and the loop stops when the sampled
input bits stabilize. Results contain both analog node traces and
`<instance>.<port>` digital traces. Mixed mode requires top-level HDL
instances; `schematic-validate-hierarchy` reports `inout` ports, buses, missing
connections, and duplicate output drivers. Use `schematic-add-hdl-instance ...
--flatten-buses` for scalar mixed simulation of a bus; pins become `data[0]`,
`data[1]`, … and are concatenated in the generated testbench. `real` ports are
bridged without logic thresholding.

`simulation-trace` returns decimated waveform samples for the latest stored run,
which avoids writing a script just to inspect `v(out)` or a digital port.

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
disk.

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
