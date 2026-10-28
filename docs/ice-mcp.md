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
| `project_summary` | List schematics, layouts, layers, and simulations. |
| `project_validate` | Return validation diagnostics. |
| `project_undo` / `project_redo` | Move through project history. |
| `schematic_create` | Create a drawn schematic cell. |
| `schematic_create_text` | Create a text SPICE schematic. |
| `schematic_add_symbol` | Add a symbol definition. |
| `schematic_add_instance` | Place a symbol instance. |
| `layout_create` | Create a static layout cell. |
| `layout_create_from_schematic` | Generate a layout from a schematic. |
| `layout_add_layer` | Add a layout layer. |
| `layout_add_rectangle` | Add a rectangle shape. |
| `layout_add_polygon` | Add a polygon shape. |
| `layout_add_port` | Add a port. |
| `layout_add_instance` | Instance another layout cell. |

Cells and layers may be referenced by name or numeric id. Coordinates are
integer database units.

Mutations are transactional in memory and recorded in project history. The MCP
server does not write them to disk. Use `ice-cli` for persisted project changes.

## Resources

| URI | Contents |
| --- | --- |
| `ice://project/summary` | JSON summary of the open project. |
| `ice://project/json` | Canonical serialized project JSON. |
| `ice://project/validation` | Validation diagnostics. |

## Dependencies

`ice-mcp` is compiled with ACP/MCP support when an `agentsdk-cpp` checkout is
found next to the ICE repository or through `AGENTSDK_CPP_DIR`. Without it the
executable reports that the MCP server is unavailable.
