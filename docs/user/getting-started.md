# Getting Started

OpenVLSI is a programmable integrated-circuit design environment. The `openvlsi` desktop
application, `openvlsi-cli`, Python bindings, MCP server, and agent tools all operate
on the same validated project model, so a design created in one interface is
immediately available in the others.

This guide is for people using OpenVLSI. If you are building, testing, or extending
OpenVLSI itself, start with the {doc}`/developer/index` instead.

## Before you begin

You need an OpenVLSI build on your `PATH`:

- `openvlsi` — the desktop application.
- `openvlsi-cli` — headless project, layout, simulation, and verification commands.
- `openvlsi-pkg` — package and PDK management.
- `openvlsi-mcp` — the optional MCP server for agents (when built/installed).

If you do not have binaries yet, follow {doc}`/developer/building`. Tagged
releases publish three packages: a Windows NSIS installer, a macOS zip with
`OpenVLSI.app`, and a Linux `tar.gz`/zip with the four executables and shared
resources. The Windows installer adds `bin` to `PATH`; on macOS move `OpenVLSI.app`
to Applications; on Linux extract the archive and add its `bin` directory to
`PATH`.

## Launch the desktop application

```sh
openvlsi
```

When no project is open, OpenVLSI shows a startup view with recent projects and
actions to create or open a project. Closing a project returns to this view.

## Create a project

Create a project from the startup view or **File → New Project**. OpenVLSI asks for
a project name, a project folder, and whether to create a folder named after
the project. The new project is initialized with:

```text
project.ovpro
sch/                 # schematic cells (*.ovsch, text SPICE, SystemVerilog)
lay/                 # layout cells (*.ovlay) and procedural layouts
impl/                # implementation runs (*.ovimpl)
sim/                 # simulations (*.ovsim)
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
openvlsi-cli --version
openvlsi-cli project-create ./demo "Demo project"
openvlsi-cli project-info ./demo
openvlsi-cli project-validate ./demo
openvlsi-cli doctor ./demo
```

`project-info` can omit the path: OpenVLSI uses `OPENVLSI_PROJECT` or searches the
current directory and its parents for a single `project.ovpro`.

See {doc}`cli` for the complete command reference.

## Install a PDK package

Simulation and package PDKs come from OpenVLSI packages. Install and verify one
before simulating:

```sh
openvlsi-pkg resolve ~/.openvlsi/packages 0.1.0 ihp.sg13g2
openvlsi-cli pdk-build ihp-sg13g2
openvlsi-cli pdk-info ihp-sg13g2 --probe
```

The bundled index provides IHP SG13G2, SKY130, and GF180MCU packages. The GUI
package manager is under **Project → Manage Packages**. See
{doc}`packages` for installation, project dependencies, and the GUI workflow.

## Automate and script

| Surface | Start with |
| --- | --- |
| Python API | {doc}`openvlsipy` |
| Procedural layout scripts | {doc}`procedural-layout` |
| MCP tools for agents | {doc}`openvlsi-mcp` |
| End-to-end agent examples | {doc}`agent-cookbook` |

## Next steps

- {doc}`gui` — desktop shell, schematic/layout editors, and shortcuts.
- {doc}`cli` — headless project and design commands.
- {doc}`simulation` — SPICE, mixed-signal, and digital simulation.
- {doc}`symbols` — built-in symbol library reference.
- {doc}`packages` — installing and managing PDK packages.
- {doc}`/project/vision` — why OpenVLSI is organized around one canonical core.
