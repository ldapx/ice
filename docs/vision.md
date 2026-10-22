# ICE Vision

## Purpose

ICE is an open-source, extensible integrated-circuit design environment for
schematics, IC layout, simulation, analysis, automated layout, and programmable
design workflows.

It should unify workflows that are normally split across separate tools while
remaining compatible with established EDA backends and interchange formats.

## Core promise

ICE has one canonical design model and one set of core operations. The main EDA
application, command-line tools, Python API, MCP server, agent chatbox, and
plugins are different interfaces to the shared `icelib` core library.

This means a design operation should not have separate implementations for each
interface. The `ice`, `ice-cli`, and `ice-mcp` executables, Python bindings,
and plugins should converge on the same validated `icelib` operation.
Interfaces may differ in interaction style, permissions, and presentation.

## Product principles

### Core-first architecture

The GUI is a client of the application core, not the primary abstraction.
Domain objects and operations must be usable without Qt so that they can be
driven by automation, tests, scripts, and agents.

### Manual and automated design are one workflow

Generated layout, manual editing, Python automation, AI-assisted changes, and
optimization must operate on the same layout database. Moving between these
workflows should not require lossy export/import cycles.

### Deterministic and reproducible results

Geometry, project state, simulations, package resolution, and generated
artifacts should be reproducible from declared inputs and recorded versions.
Design files should identify the format version even while backwards
compatibility is intentionally deferred during early development.

### Explicit boundaries

External tools and libraries should be accessed through adapters. Their types,
file formats, and implementation-specific assumptions should not leak into the
canonical ICE model.

### Extensible without fragmenting the core

PDKs, generators, simulation backends, verification tools, package types, and
AI integrations should be extensible without requiring forks of ICE or parallel
representations of a design. A stable third-party plugin API is intentionally
deferred until ICE has a substantial user and plugin base.

### Safe automation

Scripts, plugins, package code, external workers, and AI agents can mutate
valuable design data. Automation must therefore support scoped permissions,
validation, preview or dry-run behavior where appropriate, undoable changes,
and auditable results.

### Solid engineering

ICE code must be solid as rock: correct, deterministic, maintainable, and
carefully tested. C++ and Qt code should follow the
[Qt Coding Style](https://wiki.qt.io/Qt_Coding_Style), including its
conventions for naming, formatting, structure, and readability. Exceptions
should be deliberate, documented where necessary, and consistent across the
codebase.

## Intended user experience

An ICE user should be able to:

1. Create or import a schematic.
2. Select devices and configure a PDK.
3. Define reusable testbenches and run simulations.
4. Analyze and benchmark circuit behavior.
5. Generate an initial layout or create one manually.
6. Refine the layout through the GUI, Python, or an agent.
7. Run DRC, LVS, and parasitic extraction.
8. Re-run post-layout simulation and compare results.
9. Package and reproduce the project with its exact dependencies.

The first implementation does not need to support this entire workflow. It must
validate the shared-core model with a small, complete vertical slice before
adding the larger ecosystem.

## Long-term direction

ICE should become a programmable IC design environment composed of:

- the main EDA application, `ice`;
- the package manager, `ice-pkg`;
- the command-line interface, `ice-cli`;
- the MCP server, `ice-mcp`;
- and the shared core library, `icelib`.

Together these provide:

- schematic capture,
- hierarchical layout,
- simulation and model analysis,
- package and PDK management,
- verification and extraction,
- benchmarking and optimization,
- Python automation,
- MCP-based external agents,
- and an in-application ACP chatbox.

These are long-term capabilities. They should not determine the shape of the
initial implementation unless they are needed to validate a core architectural
contract.
