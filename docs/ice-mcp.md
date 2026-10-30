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

Or open a project at startup:

```sh
ice-mcp --project design/project.icepro
```

The process is silent on stdout except for JSON-RPC messages; diagnostic logs
go to stderr.

## Tools

| Tool | Purpose |
| --- | --- |
| `ice_version` | Return the ICE version. |
| `project_create` | Create and open a project file. |
| `project_open` | Open an existing project. |
| `project_summary` | List schematics, layouts, layers, simulations, and simulation scripts. |
| `project_validate` | Return validation diagnostics. |
| `project_undo` / `project_redo` | Move through project history. |
| `schematic_create` | Create a drawn schematic cell. |
| `schematic_create_text` | Create a text SPICE schematic. |
| `schematic_delete` | Delete a schematic, blocked when simulations or symbols use it. |
| `schematic_add_symbol` | Add a symbol definition. |
| `schematic_add_instance` | Place a symbol instance. |
| `layout_create` | Create a static layout cell. |
| `layout_create_from_schematic` | Generate a layout from a schematic. |
| `layout_delete` | Delete a static or procedural layout, blocked when referenced. |
| `layout_add_layer` | Add a layout layer. |
| `layout_add_rectangle` | Add a rectangle shape. |
| `layout_add_polygon` | Add a polygon shape. |
| `layout_add_port` | Add a port. |
| `layout_add_instance` | Instance another layout cell. |
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
name or numeric id. Coordinates are integer database units.

Project mutations are recorded in project history and saved automatically after
each successful tool call, so the GUI and `ice-cli` see them immediately.
Simulation script source files are written under `scripts/`.

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
