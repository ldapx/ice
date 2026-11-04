# ice-cli

`ice-cli` is the headless automation surface over the same `icelib` operations
used by the GUI, Python bindings, and MCP server. Successful commands print JSON
to stdout; diagnostics and logs go to stderr.

## Project

```sh
ice-cli project-create <folder|project.icepro> [name]
ice-cli project-info <folder|project.icepro>
ice-cli project-validate <folder|project.icepro>
ice-cli project-repair <folder|project.icepro>
ice-cli project-migrate <folder|project.icepro>
ice-cli doctor [project] [--probe]
ice-cli pdk-info [pdk] [--probe]
```

`project-create` initializes `project.icepro`, `sch/`, `lay/`, `sim/`,
`scripts/`, `build/`, `.gitignore`, and `AGENTS.md`. `project-migrate` refreshes
the versioned generated section of `AGENTS.md` and preserves user text outside
the markers.

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
package/OpenVAF status; its `--probe` flag runs the same check. `doctor` exits with status `1`
when `ok` is false, so scripts and agents can fail fast on a broken PDK setup.

## Schematic

```sh
ice-cli schematic-add-cell <project> <name> [pdk]
ice-cli schematic-add-text <project> <name> [pdk] [spice-file]
ice-cli schematic-add-text <project> <name> --pdk <pdk> --file <spice-file>
ice-cli schematic-list <project>
ice-cli schematic-info <project> <cell>
ice-cli schematic-remove <project> <cell>
ice-cli schematic-add-symbol <project> <cell> <name> [source-cell]
ice-cli schematic-add-pin <project> <cell> <symbol> <name> [x] [y] [direction]
ice-cli schematic-add-instance <project> <cell> <symbol> <name> <x> <y>
ice-cli schematic-add-net <project> <cell> <name> <pin> <pin> ...
ice-cli schematic-netlist <project> <cell>
```

Cells, symbols, and pins may be referenced by name or id. Pins can also use the
`<symbol>.<pin>` form. `schematic-add-pin` adds a pin to an existing symbol;
direction accepts `left`, `right`, `up`, or `down`. `schematic-remove` is blocked
when the cell is used by a simulation or a hierarchical symbol.

`schematic-add-text` accepts either the positional `[pdk] [spice-file]` form or
the unambiguous `--pdk <pdk> --file <spice-file>` form. If a file path is passed
where the PDK is expected, the command accepts it as a file but warns that the
cell was stored with the generic PDK; pass `--pdk` to preserve the intended PDK.
The MCP equivalent is `schematic_create` with `kind: "text"` or
`schematic_add_text`.

## Layout

```sh
ice-cli layout-add-cell <project> <name> [pdk]
ice-cli layout-add-procedural <project> <name> [pdk] [script-file]
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
ice-cli layout-export-gds <project> <cell> <output.gds>
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

## Verification

```sh
ice-cli lvs <project> <schematic> <layout>
ice-cli lvs-auto <project> <layout>
ice-cli pex <project> <layout> [schematic-name]
```

`project-repair` loads a project with recovery enabled: malformed sidecar files
are moved under `build/recovery/`, the valid cells are kept, and the project is
saved again. Use it when a hand-edited or partially written `sch/`, `lay/`, or
`sim/` file prevents normal loading. Strict load errors now include the exact
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
run. `simulate` runs a schematic directly without a simulation definition.
`simulation-remove` deletes the simulation and its graph definitions; stored
build runs remain on disk.

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
