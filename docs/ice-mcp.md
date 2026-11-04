# ice-mcp server

`ice-mcp` is ICE's Model Context Protocol server. It speaks newline-delimited
JSON-RPC over stdio using the `agentsdk-cpp` MCP server implementation and calls
the same `icelib` operations as the GUI and CLI.

## Build and run

```sh
xmake build ice-mcp
```

Run without a project and open/create one through tools:

```sh
ice-mcp
```

After building, run the MCP tool smoke test:

```sh
python3 tests/mcp_tools_smoke.py
```

It exercises drawn-schematic creation with pins and nets, parameter editing,
SystemVerilog source and port inspection, mixed-signal co-simulation, simulation
diagnostics, and doctor/PDK discovery.

The MCP server exposes `simulation_create`, `simulation_update`, and
`simulation_run`, so a client can create, configure, and run a simulation
without switching to `ice-cli`. `project_reload` explicitly refreshes an open
project; the server also detects changes made by another ICE client before a
mutation. Existing text SPICE and SystemVerilog cells can be edited with
`schematic_set_text`.

Or open a project at startup:

```sh
ice-mcp --project design/project.icepro
ice-mcp --project design/project.icepro --recover
```

If the startup project has a malformed sidecar, the server starts in a degraded
state instead of exiting, reports the warning in `ice_version`/`ice_doctor`, and
still exposes `project_open(recover=true)` and `project_repair`. `--recover`
quarantines the invalid file immediately.

The process is silent on stdout except for JSON-RPC messages; diagnostic logs
go to stderr.

## Build fingerprint and restarting

`ice_version` returns the version, git commit, build time, build type,
executable path, and feature flags. The same fingerprint is included in the MCP
instructions. Restart the MCP client after installing or rebuilding ICE:
a long-lived server keeps the binary code it loaded at process start, even when
`~/.local/bin/ice-mcp` has been replaced.

At the start of a session, call `ice_version` and confirm features such as
`project_repair`, `project_migrate`, `simulation_metrics`, and `pdk_status`.
If a required feature is missing, stop and ask the user to rebuild/restart ICE.

## Tools

| Tool | Purpose |
| --- | --- |
| `ice_version` | Return the ICE build fingerprint and feature flags. |
| `pdk_info` | Inspect ICE PDK packages, ngspice models, OSDI setup, sections, and optional transistor probe. |
| `pdk_build` | Build an ICE PDK package's Verilog-A sources into the OSDI cache. |
| `project_create` | Create and open a project file. |
| `project_open` | Open an existing project; optional `recover` quarantines malformed sidecar files. |
| `project_summary` | List schematics, layouts, layers, simulations, and simulation scripts. |
| `project_validate` | Return validation diagnostics. |
| `project_repair` | Recover a project and save it after quarantining invalid files. |
| `project_migrate` | Refresh the generated `AGENTS.md` guidance while preserving user text. |
| `project_undo` / `project_redo` | Move through project history. |
| `ice_doctor` | Inspect Python, ngspice, PDK readiness, model libraries, and optional schematic/simulation model includes. |
| `schematic_create` | Create a schematic cell; use `kind="text"` for SPICE text or `kind="systemverilog"` for a digital source cell. |
| `schematic_list` | List schematic cells and their symbol/instance/net counts. |
| `schematic_info` | Read symbols, pins, instances, parameters, and nets. |
| `schematic_create_text` | Create a text SPICE schematic. |
| `schematic_create_systemverilog` | Create a SystemVerilog source cell for digital HDL simulation. |
| `schematic_hdl_ports` | Parse a SystemVerilog cell and return its module name, port directions, and widths. |
| `schematic_delete` | Delete a schematic, blocked when simulations or symbols use it. |
| `schematic_add_symbol` | Add a symbol definition, optionally with a `pins` array. |
| `schematic_add_pin` | Add a pin to an existing symbol. |
| `schematic_add_net` | Connect two or more pin ids or `<instance>.<pin>` names into a net. |
| `schematic_netlist` | Return the SPICE netlist generated from a schematic. |
| `schematic_add_instance` | Place a symbol instance by id or symbol name. |
| `schematic_add_hdl_instance` | Generate a symbol from a SystemVerilog module's ports and place it. |
| `schematic_set_parameter` | Set or create an instance parameter such as `resistance` or `value`. |
| `schematic_validate_hierarchy` | Validate HDL instances, pin/port mapping, nets, buses, and mixed-signal support. |
| `layout_create` | Create a static layout cell. |
| `layout_create_from_schematic` | Generate a layout from a schematic. |
| `layout_delete` | Delete a static or procedural layout, blocked when referenced. |
| `layout_add_layer` | Add a layout layer with optional GDS datatype/purpose. |
| `layout_add_rectangle` | Add a rectangle shape. |
| `layout_add_text` | Add text to a layout cell on a layer. |
| `layout_add_polygon` | Add a polygon shape. |
| `layout_add_port` | Add a pin with required direction/side/label metadata and optional net. |
| `layout_list_ports` | List pins and nets for a layout cell. |
| `layout_update_port` | Update pin geometry, direction, label, visibility, lock state, or net. |
| `layout_remove_port` | Remove a layout pin. |
| `layout_add_net` | Create a layout net. |
| `layout_assign_port_net` | Assign or clear a pin’s net. |
| `layout_add_instance` | Instance another layout cell. |
| `layout_info` | Read a layout cell's pins, nets, instances, and shape count. |
| `layout_connectivity` | Extract nets from explicit layout nets, same-layer geometry, and via/contact overlaps. |
| `layout_export_gds` | Export a layout cell to GDSII, including pins and labels. |
| `layout_export_lef` | Export pins as a minimal LEF macro. |
| `layout_export_def` | Export pins and nets as a minimal DEF design. |
| `lvs_run` | Compare a layout against a schematic and return the LVS report, including top-level pin matches. |
| `pex_run` | Extract placed and PDK-rule polygon devices plus first-order parasitics, then create or update a text SPICE schematic named `<layout> PEX`. |
| `simulation_result` | Read the latest stored run with trace statistics and signal diagnostics. |
| `simulation_signals` | List available signals and trace sizes from the latest stored run. |
| `simulation_measure` | Measure crossings, frequency, amplitude, RMS, and settling time. |
| `simulation_graph_create` | Add a graph definition to a simulation. |
| `simulation_graph_list` | List graph definitions. |
| `simulation_graph_info` | Read one graph definition. |
| `simulation_graph_update` | Update axes, signals, or limits. |
| `simulation_graph_delete` | Delete a graph while keeping the last one. |
| `simulation_delete` | Delete a simulation and all its graph definitions. |
| `simulation_script_create` | Create a simulation script from a template or supplied source. |
| `simulation_script_list` | List project-level simulation scripts. |
| `simulation_script_read` | Read simulation script metadata and Python source. |
| `simulation_script_write` | Replace simulation script source. |
| `simulation_script_run` | Run a script and return output plus generated graphs. |
| `simulation_script_delete` | Delete script metadata; the file remains. |

Cells, layers, simulations, graphs, and simulation scripts may be referenced by
name or numeric id. Coordinates are integer database units. `lvs_run` accepts a
layout name or id and an optional schematic; when the schematic is omitted, ICE
infers it from planned-device links or matching layout/schematic names.
`pex_run` accepts a layout plus optional coefficient overrides
(`metal_sheet_resistance`, `poly_sheet_resistance`,
`diffusion_sheet_resistance`, `via_resistance`, `area_capacitance`,
`perimeter_capacitance`) and an optional `name` for the generated text cell.
It extracts placed device-library instances by default; set
`extract_placed_devices=false` to skip them. It also recognizes polygon devices
through the PDK's `layout/extraction.json`; set `extract_polygon_devices=false`
to disable that pass, or `extraction_rules_path` to override the package file.
If no layout devices are recognized, it copies the inferred source schematic
netlist by default; set `include_linked_schematic=false` to emit only the
extracted RC network. The response includes `devices`, `devices_from_layout`,
and `devices_from_schematic`.

A drawn schematic can be created and connected entirely through MCP:
`schematic_add_symbol` can create a symbol with its pins, `schematic_add_instance`
accepts either `symbol_id` or `symbol`, and `schematic_add_net` accepts pin ids
or readable names such as `I1.Y` and `I2.A`. Use `schematic_info` to inspect the
result and `schematic_netlist` to check the generated SPICE. Duplicate symbol
names and duplicate pin names are rejected.

`simulation_run` returns the state, error, netlist context, trace statistics,
and available signal names. A simulation that points at a SystemVerilog cell
uses the `iverilog` simulator and `digital` analysis automatically; the result
traces come from the generated VCD, and empty capture/graph selections are
filled from the first traces and saved. If ngspice or a PDK library cannot initialize, run
`ice_doctor` with the schematic or simulation name to validate model includes
and PDK readiness before editing the circuit. `simulation_result` and
`simulation_signals` read the latest stored run without requiring shell access
to `build/`. `simulation_measure` computes min/max/mean/RMS, zero crossings,
rising crossings, period, frequency, amplitude, and settling time for one or all
captured signals.

A successful transient run is not proof of oscillation. Use
`simulation_measure` and report the measurements before claiming that an
oscillator works.

## PDK readiness

Simulation PDKs come from ICE packages only. Volare, CIEL, `PDK_ROOT`, `PDK`,
and global `~/.spiceinit` files are not used.

`ice_doctor` now includes per-PDK readiness, and `pdk_info` returns the package
name/version, exact ngspice model directory, `.lib` sections, OSDI files,
OpenVAF availability, ordered `recommended_setup` lines, and example devices.
`pdks_ready` is true when at least one installed PDK package is ready;
`all_pdks_ready` is true only when every installed package is ready. When a
project uses a PDK that is not simulation-ready, `ice_doctor` returns `ok: false`
with a `pdk_hint` naming the missing pieces. Pass `probe: true` to `ice_doctor`
to run a minimal transistor load check. Do not replace a transistor-level
request with a behavioral model silently; report the blocker and ask the user.

OSDI PDKs need a one-time local build:

```text
pdk_build pdk="ihp-sg13g2"
```

This compiles the package Verilog-A sources into the ICE OSDI cache. ICE then
loads those files explicitly for each simulation; no global `.spiceinit` is
needed.

For IHP SG13G2, the correct model entry is a top-level call such as:

```spice
.lib "<package>/libs.tech/ngspice/models/cornerMOSlv.lib" mos_tt
```

not `.include cornerMOSlv.lib`. Sky130 and GF180 use different layouts
(`sky130.lib.spice` with `.option scale=1u`, and `design.ngspice` plus
`sm141064.ngspice`); `pdk_info` returns the ordered `recommended_setup` lines and
example device names. Use `pdk_info` with `probe: true` for an explicit
transistor-level load check.

## Source schematics

Use `schematic_create` with `kind: "text"` (or the `schematic_create_text` /
`schematic_add_text` aliases) when a text SPICE cell is intended. Use
`kind: "systemverilog"` or `schematic_create_systemverilog` for a digital HDL
cell. Do not create a drawn cell and then write its sidecar by hand.

`schematic_set_text` replaces the source of an existing text SPICE or
SystemVerilog cell; pass `kind` only when its language must be explicit. It can
also convert an empty drawn cell when called with `convert: true`; non-empty
drawn cells are rejected. A source cell cannot switch between SPICE and
SystemVerilog in place.

SystemVerilog cells are simulated with Icarus Verilog and the `digital`
analysis. `simulation_create` selects those defaults for such a cell, and
`simulation_run` returns VCD-derived traces. If the testbench does not create a
VCD, ICE adds a wrapper that dumps the design. Source-level `$dumpfile` and
`$dumpvars` calls are respected.

## Mixed-signal schematics

`kind: "systemverilog"` cells are digital blocks. Place one in a drawn
schematic with `schematic_add_hdl_instance`; ICE parses the module ports and
generates a box symbol with matching pins. `schematic_add_symbol` also
auto-generates pins and artwork when it is given a SystemVerilog `source_cell`
without an explicit `pins` array. Connect pins with readable names such as
`U1.in` and `U1.out` via `schematic_add_net`. Set analog instance values with
`schematic_set_parameter`, for example `resistance="1k"` or
`value="PULSE(0 1 5n 1n 1n 20n 40n)"`.

Call `schematic_hdl_ports` to inspect the parsed module and
`schematic_validate_hierarchy` to check pin/port names, missing nets, bus
widths, `inout` ports, and duplicate output drivers. Pure SPICE netlisting
(`schematic_netlist`, or analog `simulation_run`) rejects hierarchical
SystemVerilog blocks explicitly instead of silently dropping them.

A drawn schematic containing HDL blocks is detected as mixed-signal.
`simulation_create` selects `simulator="mixed"` and `transient` analysis
automatically. Mixed simulation iterates ngspice and Icarus Verilog:
digital outputs drive PWL sources in the analog deck, analog input nodes are
sampled through a logic threshold, and the loop stops when sampled input bits
stabilize. Results contain analog node traces plus `<instance>.<port>` digital
traces. The current implementation supports top-level HDL instances with scalar
`input`/`output` ports.

Project sidecar files are not an editing interface. Do not write
`sch/*.icesch`, `lay/*.icelay`, or `sim/*.icesim` by hand; use the tools instead.
If one becomes malformed, call `project_repair` or `project_open` with
`recover: true`. The recovered file is moved under `build/recovery/`. After
upgrading ICE, call `project_migrate` to refresh the generated `AGENTS.md`
guidance; user text outside the generated markers is preserved.

Project mutations are recorded in project history and saved automatically after
each successful tool call, so the GUI and `ice-cli` see them immediately.
Simulation script source files are written under `scripts/`.

Sidecar and metadata writes are atomic, and saves use a project lock plus a
revision check. If another ICE process changed the project after this server
loaded it, the stale save fails with `project.changed` instead of overwriting
the newer files. Reload or reopen the project and retry.

`simulation_script_run` executes the script through the shared Python runner and
returns `exit_code`, `timed_out`, `stdout`, `stderr`, and generated graph data,
so agents can inspect analysis results and adjust the schematic in the same
session. The legacy `report_*` tool names remain accepted as aliases.

## Resources

| URI | Contents |
| --- | --- |
| `ice://project/summary` | JSON summary of the open project. |
| `ice://project/simulation-scripts` | Simulation script metadata and Python sources. |
| `ice://project/json` | Canonical serialized project JSON. |
| `ice://project/validation` | Validation diagnostics. |

## Dependencies

`ice-mcp` is built against the bundled `third_party/agentsdk-cpp` submodule as a
shared `libagentsdk` library. Initialize the submodule with
`git submodule update --init --recursive`; the installed executable loads the
library from the installation's `lib` directory.
