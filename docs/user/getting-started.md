# Getting Started

ICE is a programmable integrated-circuit design environment. The `ice` desktop
application, `ice-cli`, Python bindings, MCP server, and agent tools all operate
on the same validated project model, so a design created in one interface is
immediately available in the others.

This guide is for people using ICE. If you are building, testing, or extending
ICE itself, start with the {doc}`/developer/index` instead.

## Before you begin

You need an ICE build on your `PATH`:

- `ice` — the desktop application.
- `ice-cli` — headless project, layout, simulation, and verification commands.
- `ice-pkg` — package and PDK management.
- `ice-mcp` — the optional MCP server for agents (when built/installed).

If you do not have binaries yet, follow {doc}`/developer/building`.

## Launch the desktop application

```sh
ice
```

When no project is open, ICE shows a startup view with recent projects and
actions to create or open a project. Closing a project returns to this view.

## Create a project

Create a project from the startup view or **File → New Project**. ICE asks for
a project name, a project folder, and whether to create a folder named after
the project. The new project is initialized with:

```text
project.icepro
sch/                 # schematic cells (*.icesch, text SPICE, SystemVerilog)
lay/                 # layout cells (*.icelay) and procedural layouts
impl/                # implementation runs (*.iceimpl)
sim/                 # simulations (*.icesim)
scripts/             # project-level Python scripts
build/               # generated outputs and simulation results
.gitignore
AGENTS.md
```

Opening a project validates its schema, package dependencies, and PDK
availability. Missing packages are reported before the project opens.

## Find your way around

The document tab strip groups the main workspaces:

| Workspace | Use it for |
| --- | --- |
| **A/MS Schematic** | Drawn analog/mixed-signal schematics. |
| **HDL Schematic** | SystemVerilog source cells and digital hierarchy. |
| **Synthesis** | Digital synthesis setup and results. |
| **Physical Design** | Floorplan, placement, CTS, routing, and final layout stages. |
| **Layout** | Manual layout editing, procedural layouts, DRC, and LVS. |
| **A/MS Simulation** | SPICE and mixed-signal simulation setup and waveforms. |
| **Digital Testbenches** | SystemVerilog testbenches and digital simulation. |

The left side shows the project hierarchy and domain-specific panels; the
right side shows properties and diagnostics. Long-running jobs run in the
background and report progress in the Messages dock.

See {doc}`gui` for the full desktop workflow, shortcuts, and editor contracts.

## Use the command line

Every command prints JSON on success and diagnostics on stderr. Start with:

```sh
ice-cli --version
ice-cli project-create ./demo "Demo project"
ice-cli project-info ./demo
ice-cli project-validate ./demo
ice-cli doctor ./demo
```

`project-info` can omit the path: ICE uses `ICE_PROJECT` or searches the
current directory and its parents for a single `project.icepro`.

See {doc}`cli` for the complete command reference.

## Install a PDK package

Simulation and package PDKs come from ICE packages. Install and verify one
before simulating:

```sh
ice-pkg resolve ~/.ice/packages 0.1.0 ihp.sg13g2
ice-cli pdk-build ihp-sg13g2
ice-cli pdk-info ihp-sg13g2 --probe
```

The bundled index provides IHP SG13G2, SKY130, and GF180MCU packages. The GUI
package manager is under **Project → Manage Packages**. See
{doc}`packages` for installation, project dependencies, and the GUI workflow.

## Automate and script

| Surface | Start with |
| --- | --- |
| Python API | {doc}`icepy` |
| Procedural layout scripts | {doc}`procedural-layout` |
| MCP tools for agents | {doc}`ice-mcp` |
| End-to-end agent examples | {doc}`agent-cookbook` |

## Next steps

- {doc}`gui` — desktop shell, schematic/layout editors, and shortcuts.
- {doc}`cli` — headless project and design commands.
- {doc}`simulation` — SPICE, mixed-signal, and digital simulation.
- {doc}`symbols` — built-in symbol library reference.
- {doc}`packages` — installing and managing PDK packages.
- {doc}`/project/vision` — why ICE is organized around one canonical core.
