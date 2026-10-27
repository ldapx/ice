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
| Generate IHP PCell geometry | Yes | Devices panel | Core API | `icepy.pcells.generate_cell` |
| Polygon Boolean/offset | Yes | Geometry kernel | Core API | Core API |
| Spatial query | Yes | Layout integration | Core API | Core API |

Python bindings are an optional `icepy` build surface because nanobind and
Python development headers are not required by the core, GUI, or CLI builds.
The module supports project creation/loading, identity and schema inspection,
layout-cell and layer inspection, mutation through a layout builder, validation,
and an IHP PCell runtime used by the IHP package's device generator.

The current geometry implementation owns all public polygon and transform
types. It uses deterministic integer operations and does not expose a Clipper2
type in any public header; a full external polygon backend remains an isolated
follow-up once the dependency is available in the supported build matrix.
