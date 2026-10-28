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
`reports/`, `build/`, `.gitignore`, and `AGENTS.md`.

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
ice-cli simulate <project> <schematic> <output.json> [stop] [step] [timeout-ms] [signals]
ice-cli simulate-netlist <netlist-file> <output.json> [stop] [step] [timeout-ms] [signals]
```

Time values accept SPICE-style engineering suffixes such as `1u`, `100n`, and
`2m`. Signals are comma-separated names such as `1,2` or `out,v(out)`.

Simulation results are written as JSON using the core simulation result schema.
