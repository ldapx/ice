# ICE Interface Capability Matrix

`icelib` project operations are exposed through the command-line
and desktop surfaces. The GUI views are projections of the canonical model; the
CLI invokes the same operations through `ProjectSession`.

| Operation | Core | GUI | CLI | Python |
| --- | --- | --- | --- | --- |
| Create/open/validate project | Yes | Startup shell | Yes | `icepy.create_project`, `icepy.load_project` |
| Inspect layout cells/layers | Yes | Layout surface | Core API | `Project.layout_cells`, `Project.layers` |
| Create layout cell | Yes | Layout surface | Yes | `Project.create_layout_cell` |
| Add rectangle/polygon/port | Yes | Layout surface | Yes | `LayoutBuilder` |
| Undo/redo transaction | Yes | Core-backed | Session-backed | Core-backed |
| Generate package PCell geometry | Yes | Devices panel | Core API | `icepy.pcells.generate_cell`, `icepy.pcells.generate_package_geometry` |
| Polygon Boolean/offset | Yes | Layout toolbar (Boolean) | Core API | Core API |
| Layout-versus-schematic check | Yes | Tools → Run LVS | `ice-cli lvs` | `icepy.run_lvs` |
| Mixed-signal HDL capture | Yes | Schematic + Simulation surfaces | `schematic-add-hdl-instance`, `simulation-run` | Core API |
| Mixed-signal co-simulation | Yes | Mixed-signal simulator | `simulation-run` | Core API |
| Spatial query | Yes | Layout integration | Core API | Core API |

Python bindings are an optional `icepy` build surface because nanobind and
Python development headers are not required by the core, GUI, or CLI builds.
The module supports project creation/loading, identity and schema inspection,
layout-cell and layer inspection, mutation through a layout builder, validation,
an IHP PCell runtime used by the IHP package's device generator, and a generic
package PCell runtime used by the Sky130 and GF180MCU ICE-native generators.

The geometry implementation owns all public polygon and transform types. It
uses deterministic integer operations and delegates polygon Boolean operations
and offsetting to Clipper2 behind the `GeometryKernel` adapter; no Clipper2 type
appears in a public header.
