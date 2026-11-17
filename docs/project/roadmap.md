# ICE Implementation Roadmap

This roadmap records the capabilities, contracts, and acceptance criteria
for ICE. Each area must preserve the core architectural boundaries. Work not
listed here is deferred.

## Foundation

### Objective

Establish the build, test, logging, error, configuration, and repository
foundations needed for a maintainable C++/Qt application.

### Scope

- `icelib` C++ core library target.
- `ice` Qt 6 desktop application target.
- `ice-cli` headless CLI target.
- `ice-pkg` package-manager target.
- `ice-mcp` MCP-server target.
- xmake build configurations for the library, executables, and tests.
- doctest test target.
- structured logging with spdlog.
- CLI entry point and version reporting.
- project error model and process-exit conventions.
- formatting, documentation, and CI conventions.

### Acceptance criteria

- A clean checkout configures and builds the application.
- Unit tests run without launching the GUI.
- CLI version/help commands work.
- A failing command returns a structured error and nonzero exit status.
- The GUI can start without a project and reports configuration errors clearly.
- All four executables link against the same `icelib` target.

## Canonical project and domain model

### Objective

Prove the central claim: GUI, CLI, and Python-facing operations can use one
validated domain model.

### Scope

- Project identity and stable object IDs.
- Project file loading and saving with explicit schema/version metadata.
- Schematic cells, symbols, pins, instances, nets, and parameters.
- Layout cells, layers, shapes, ports, and instances.
- Integer database units and coordinate conversion rules.
- Mutation commands with validation.
- Transactions, undo/redo, change notifications, and dirty-state tracking.
- Deterministic serialization tests.

### Acceptance criteria

- A project containing a minimal schematic and layout round-trips through disk.
- Invalid references and off-grid data are rejected with actionable diagnostics.
- The same create/connect/edit operation can be invoked from core tests and CLI.
- Undo/redo restores exact model state.
- Serialization output is deterministic for equivalent project state.

## GUI, CLI, and Python surface

### Objective

Expose the same core operations through `ice`, `ice-cli`, and Python bindings
over `icelib`.

### Scope

- Qt schematic and layout views for the canonical model.
- Selection and cross-probing by stable object ID.
- CLI commands for project, schematic, and layout mutations.
- nanobind bindings for the supported domain operations.
- Python examples and API documentation.
- Capability matrix defining GUI, CLI, and Python parity.

### Acceptance criteria

- A minimal design can be created and edited through each interface.
- `ice`, `ice-cli`, and Python produce equivalent model changes for the same
  operation.
- Python can inspect, mutate, save, and reload a project.
- UI state is not required to run core or CLI tests.

## Geometry and layout editing

### Objective

Make the layout model useful for real geometry operations without coupling ICE
to Clipper2 types.

### Scope

- ICE-owned geometry kernel interface.
- Clipper2 adapter.
- Polygon validation and cleanup.
- Boolean and offset operations.
- hierarchical transforms and arrays.
- spatial index integrated with the layout database.
- snapping, grid, layer visibility, and basic editing tools.
- geometry property-based or corpus tests.

### Acceptance criteria

- Boolean and offset results are deterministic and validated.
- Cursor and rectangle queries return the expected objects.
- Hierarchical instances render and query correctly under all supported transforms.
- No Clipper2 type appears in public domain or plugin headers.

## Simulation and experiments

### Objective

Provide one complete, scriptable simulation workflow.

### Scope

- libngspice adapter and worker lifecycle.
- testbench model for one or more initial analyses.
- asynchronous execution, cancellation, timeout, and failure reporting.
- callback marshalling and serialized simulator access.
- machine-readable result storage.
- Python API for testbench execution and measurements.
- experiment/sweep abstraction.

### Acceptance criteria

- A deterministic example testbench runs from CLI and Python.
- GUI remains responsive while simulation runs.
- Cancellation and simulator failure produce explicit terminal states.
- Results can be reopened without rerunning the simulation.
- The exact simulator and model inputs are recorded with results.

## PDKs, packages, and interchange

### Objective

Make technology data and reusable design resources installable and reproducible.

### Scope

- Versioned PDK contract for layers, units, devices, models, and rules.
- Package metadata, dependency resolution, lockfile, and content hashes.
- Trusted repository and package verification model.
- Symbol/library/model package loading.
- GDSII/OASIS adapters.
- Project provenance manifest.

### Acceptance criteria

- A minimal example PDK can be installed into an isolated project.
- Lockfile resolution is deterministic and includes content hashes.
- Untrusted package code cannot run without explicit permission.
- Import/export preserves the documented supported subset.
- Project diagnostics identify missing or incompatible package capabilities.

## Verification and automated layout

### Objective

Connect layout, PDK rules, external synthesis, verification, and post-layout
simulation without contaminating the core model.

### Scope

- versioned ALIGN request/result adapter.
- DRC adapter and normalized violation model.
- LVS adapter and normalized comparison model.
- PEX adapter and normalized parasitic model.
- incremental/local verification scheduling.
- post-layout simulation integration.
- manual refinement of generated layout.

### Acceptance criteria

- ALIGN can be mocked and replaced without changing the core database.
- Verification results identify severity, location, source object, and rule.
- A minimal layout can proceed through generation, DRC, extraction, and simulation.
- Generated and manually edited geometry share the same undoable model.

## Extensions and AI interfaces

### Objective

Expose safe, structured automation to plugins and agents after the core
operation model is stable.

### Scope

- experimental plugin loading and lifecycle.
- plugin target-build and dependency declarations.
- provisional in-process versus out-of-process plugin policy.
- `ice-mcp` server over stdio JSON-RPC.
- resource, prompt, progress, cancellation, and error contracts.
- ACP chatbox with permission prompts and session state.
- transactional agent operations, dry runs, previews, and audit events.

### Acceptance criteria

- An MCP client can inspect and mutate a test project using core operations.
- Destructive agent changes require the configured approval policy.
- Agent operations are undoable and attributable to a session.
- Plugins cannot silently bypass project validation or permission checks.
- MCP and ACP failures do not corrupt the project database.

Stable plugin API/ABI versioning, compatibility windows, and migration tooling
are intentionally deferred until ICE has a substantial user and plugin base.

## Cross-cutting requirements

Every area should address:

- deterministic tests for new domain behavior,
- explicit error and cancellation states,
- schema/version metadata,
- logging and diagnostics,
- thread-safety at process boundaries,
- solid-as-rock implementation quality and adherence to the
  [Qt Coding Style](https://wiki.qt.io/Qt_Coding_Style),
- security and permission implications,
- documentation for public contracts,
- and backward-incompatible changes during the pre-user development period.

## Deferred work

- full analog optimization loops,
- Monte Carlo and yield optimization,
- complete DRC/LVS/PEX engine implementations,
- broad plugin ecosystems,
- community package repositories,
- machine-learning-assisted design,
- and fully autonomous AI layout editing.

These remain valid parts of the vision, but should not be used as prerequisites
for the first vertical slice.
