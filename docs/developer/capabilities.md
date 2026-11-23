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
| Generate package PCell geometry | Yes | Devices panel | Core API | package `ihp_runtime.generate_cell`, `icepy.pcells.generate_package_geometry` |
| Polygon Boolean/offset | Yes | Layout toolbar (Boolean) | Core API | Core API |
| Layout-versus-schematic check | Yes | Tools → Run LVS | `ice-cli lvs` | `icepy.run_lvs` |
| LVS against text SPICE or HDL | Yes | Tools → Run LVS | `ice-cli lvs` | Core API |
| Mixed-signal HDL capture | Yes | Schematic + Simulation surfaces | `schematic-add-hdl-instance`, `simulation-run` | Core API |
| Mixed-signal co-simulation | Yes | Mixed-signal simulator | `simulation-run` | Core API |
| SPICE analyses (DC, AC, transient, OP, noise, TF) | Yes | Analysis selector + experiment dialog | `simulation-configure`, `simulation-run` | Core API |
| Corners, sweeps, and Monte Carlo | Yes | Experiment dialog | `simulation-corner-*`, `simulation-sweep-*`, `simulation-monte-carlo`, `simulation-experiment` | Core API |
| Declarative measurements | Yes | Experiment dialog + Messages | `simulation-measure-add`, `simulation-measure` | Core API |
| Interactive waveform analysis | Yes | Graph zoom/pan, cursors, FFT/dB/phase, derived traces, run overlays, markers | `simulation-graph-update`, `simulation-graph-derive`, `simulation-graph-overlay`, `simulation-graph-marker-add`, `simulation-runs` | `buildWaveformSeries`, `computeSpectrum` |
| High-level component authoring | Yes | Schematic canvas | `schematic-add-component`, `schematic-add-circuit` | Core API |
| Parameterized schematic cells | Yes | Schematic Properties parameters | `schematic-set-cell-parameter`, `schematic-remove-cell-parameter`, `schematic-set-parameter` | `setSchematicCellParameter`, `setSchematicParameter` |
| Schematic editing | Yes | Schematic canvas | Schematic commands | `Project.create_schematic_cell`, `SchematicBuilder` |
| Schematic hierarchy wrap | Yes | Wrap into Symbol | `schematic-wrap` | `SchematicBuilder.wrap` |
| Layout shape duplicate / array / arrange | Yes | Duplicate, Create Array, Arrange | `layout-duplicate`, `layout-array`, `layout-arrange` | `LayoutBuilder.duplicate`, `LayoutBuilder.array`, `LayoutBuilder.arrange` |
| Schematic validation | Yes | Diagnostics | `schematic-validate` | Core API |
| Simulation configuration and execution | Yes | Analysis selector + experiment dialog | Simulation commands | `Project.create_simulation`, `run_simulation` |
| SPICE import and hierarchy | Yes | Schematic import | `schematic-import-spice`, `schematic-add-spice-instance` | Core API |
| Stored waveform sampling | Yes | Graph/result inspection | `simulation-trace` | Core API |
| Create/inspect implementation runs | Yes | Physical Design | `implementation-create`, `implementation-update`, `implementation-list`, `implementation-status` | `Project.create_implementation`, `Project.implementation_runs` |
| Prepare and execute OpenROAD/Yosys stages | Yes | Physical Design (run-to-stage per row / Run all / Cancel) | `implementation-prepare`, `implementation-map`, `implementation-run`, `implementation-log`, `implementation-cancel` | `Project.run_implementation` |
| Create a digital device from final GDS/OASIS | Yes | CLI/MCP/Python | `implementation-import` | `Project.import_implementation_layout` |
| Stream final GDS | Yes | Physical Design artifacts | `implementation-export-gds` | Core API |
| Run DRC with a PDK deck | Yes | Tools → Run DRC | `drc` | `run_layout_drc`, `LocalVerificationAdapter` |
| Package discovery, install, and lockfiles | Yes | Package manager | `ice-pkg` | `PackageManager` |
| Import/export GDSII or OASIS hierarchy | Yes | Layout interchange | `layout-import-gds`, `layout-export-gds` | Core API |
| Spatial query | Planned | Layout integration | Core API | Core API |

Python bindings are an optional `icepy` build surface because nanobind and
Python development headers are not required by the core, GUI, or CLI builds.
The module supports project creation/loading, identity and schema inspection,
layout-cell and layer inspection, mutation through a layout builder, validation,
implementation-run creation/execution/DEF import, an IHP PCell runtime used by
the IHP package's device generator, and a generic package PCell runtime used by
the Sky130 and GF180MCU ICE-native generators.

The geometry implementation owns all public polygon and transform types. It
uses deterministic integer operations and delegates polygon Boolean operations
and offsetting to Clipper2 behind the `GeometryKernel` adapter; no Clipper2 type
appears in a public header.
