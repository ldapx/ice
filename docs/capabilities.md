# ICE Interface Capability Matrix

`icelib` project operations are exposed through the command-line
and desktop surfaces. The GUI views are projections of the canonical model; the
CLI invokes the same operations through `ProjectSession`.

| Operation | Core | GUI | CLI | Python |
| --- | --- | --- | --- | --- |
| Create/open/validate project | Yes | Startup shell | Yes | `icepy.create_project`, `icepy.load_project` |
| Create schematic cell | Yes | View surface | Yes | Core-backed binding surface |
| Create layout from schematic | Yes | Schematic hierarchy context menu | Core API | Core-backed binding surface |
| Add layout rectangle | Yes | Layout surface | Yes | Core-backed binding surface |
| Undo/redo transaction | Yes | Core-backed | Session-backed | Core-backed |
| Polygon Boolean/offset | Yes | Geometry kernel | Core API | Core API |
| Spatial query | Yes | Layout integration | Core API | Core API |

Python bindings are an optional `icepy` build surface because nanobind and
Python development headers are not required by the core, GUI, or CLI builds.
The module currently supports project creation/loading, identity inspection,
schema inspection, and validation.

The current geometry implementation owns all public polygon and transform
types. It uses deterministic integer operations and does not expose a Clipper2
type in any public header; a full external polygon backend remains an isolated
follow-up once the dependency is available in the supported build matrix.
