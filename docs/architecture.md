# ICE Architecture

> Product intent is in {doc}`vision`; capabilities and acceptance
> criteria are in {doc}`roadmap`.

## Overview

ICE is an open-source, extensible **integrated-circuit design environment** focused on schematic design, IC layout, analog simulation, automated layout, analysis, benchmarking, and programmable design workflows.

ICE is a unified product composed of several executables and one shared core
library. The executables provide focused process boundaries while reusing the
same domain model and application services from `icelib`.

```text
                         ICE product
                              │
              ┌───────────────┼───────────────┐
              │               │               │
             ice           ice-cli         ice-pkg
        main EDA GUI     automation CLI   package manager
              │               │               │
              └───────────────┼───────────────┘
                              │
                           icelib
                    canonical core library
                              │
                           ice-mcp
                         MCP server
```

The executables, Python bindings, MCP server, and plugins must operate on the
same `icelib` project/database model rather than implementing independent
domain logic.

---

## 1. Goals

ICE should provide an integrated workflow for:

1. Creating and editing schematics.
2. Creating and editing IC layouts.
3. Running SPICE simulations.
4. Creating reusable analog testbenches.
5. Performing parameter sweeps and experiments.
6. Analyzing device and circuit models.
7. Benchmarking analog designs.
8. Automatically generating analog layouts.
9. Manually modifying automatically generated layouts.
10. Running DRC/LVS/PEX workflows.
11. Importing and exporting industry-standard layout formats.
12. Installing and managing PDKs, libraries, symbols, models, plugins, scripts, and other packages.
13. Automating all editor functionality through a CLI.
14. Automating ICE through Python.
15. Allowing AI agents to operate ICE through MCP.
16. Providing an Agent Client Protocol chat interface inside ICE.
17. Supporting third-party extensions without making them part of the ICE core.

---

## 2. High-Level Architecture

```text
                                      ICE
                                       │
             ┌─────────────────────────┼─────────────────────────┐
             │                         │                         │
             ▼                         ▼                         ▼
            ice                     ice-cli                    Python
         Qt 6 GUI                CLI11 frontend              bindings
             │                         │                         │
             └─────────────────────────┼─────────────────────────┘
                                       │
                                icelib Core API
                                       │
       ┌───────────────┬───────────────┼───────────────┬───────────────┐
       │               │               │               │               │
       ▼               ▼               ▼               ▼               ▼
   Schematic        Layout         Simulation      Analysis        Packages
   Database         Database         Engine         Engine          Manager
       │               │               │               │
       │               │               │               │
       │            Clipper2        ngspice         Python
       │               │               │          scientific stack
       │               │               │
       └───────────────┴───────────────┴───────────────┘
                                       │
                               External systems
                                       │
                ┌──────────────────────┼──────────────────────┐
                │                      │                      │
              ALIGN                 PDKs                GDS/OASIS
                │                      │                      │
         Analog layout           Technology data        Layout exchange
```

Separate product services use the same core:

```text
                         icelib
                            │
                ┌───────────┴───────────┐
                │                       │
             ice-mcp                 ice-pkg
          MCP JSON-RPC          package/PDK manager
                │
            AI agents
```

---

## 3. Product Executables and Process Boundaries

ICE should not be one executable. It should consist of the following primary
executables:

| Executable | Responsibility |
| --- | --- |
| `ice` | Main Qt desktop EDA application: schematic, layout, simulation, analysis, and integrated project workflows. |
| `ice-cli` | Headless automation and scripting entry point for core operations. |
| `ice-pkg` | Package and PDK installation, resolution, verification, repository, and lockfile management. |
| `ice-mcp` | MCP server exposing approved `icelib` operations to external agents. |

All four executables link to or otherwise use `icelib`. They must not maintain
separate domain models. Python bindings should also be generated from
`icelib`, and the `ice` application may host the in-process ACP chatbox.

The main `ice` application contains:

```text
ICE
├── Project Manager
├── Schematic Editor
├── Layout Editor
├── Simulation Environment
├── Model Analysis
├── Benchmarking
├── Python Console / Scripting
├── Package Manager Client
├── DRC / LVS / PEX interfaces
└── Agent Client Protocol Chatbox
```

`ice-pkg` remains a separate executable because package installation and
repository operations have different permissions, lifecycles, and failure
boundaries from interactive editing. `ice-mcp` remains separate so an external
agent connection cannot require the GUI process.

The schematic and layout editors, `ice-cli`, `ice-pkg`, and `ice-mcp` should
share through `icelib`:

* Project state
* PDK information
* Device definitions
* Libraries
* Hierarchy
* Net information
* Selection
* Cross-probing
* Simulation results
* Analysis results
* Python API
* CLI API
* AI automation API

For example:

```text
Select transistor in schematic
        │
        ├── highlight corresponding layout device
        │
        └── highlight corresponding simulation/model data
```

And:

```text
Select layout shape
        │
        ├── identify device/net
        ├── identify schematic object
        └── show relevant PDK information
```

## Process-boundary diagram

```text
                         External AI agent
                                │ stdio JSON-RPC
                                ▼
                           ┌─────────┐
                           │ ice-mcp │
                           └────┬────┘
                                │
                                ▼
┌──────────────┐      ┌──────────────────────┐      ┌──────────────┐
│     ice      │      │       icelib         │      │   ice-cli    │
│ Qt GUI/ACP   │◄────►│ domain + operations  │◄────►│ headless CLI  │
└──────┬───────┘      └──────────┬───────────┘      └──────────────┘
       │                         │
       │                         ├── Python bindings
       │                         ├── geometry/PDK adapters
       │                         └── job and persistence services
       │
       └───────────────┐
                       ▼
                 ┌──────────┐
                 │  ice-pkg │
                 │ packages │
                 └────┬─────┘
                      │
                      ▼
             repositories, PDKs, lockfiles

Optional workers are separate processes where isolation or tool lifecycle
requires it:

  ice / ice-cli / ice-mcp
              │
              ├── ngspice worker
              ├── ALIGN worker
              ├── DRC/LVS/PEX workers
              └── plugin or package worker
```

`icelib` owns the canonical model and operation contracts. Workers communicate
through versioned adapter protocols and return normalized results. A worker
failure must not corrupt the caller's project state.

---

## 4. Core Design Principle

The GUI should **not** be the primary abstraction.

`icelib` should provide the reusable application API:

```text
                       icelib API
                          │
          ┌───────────────┼───────────────┐
          │               │               │
         GUI             CLI           Python
          │               │               │
          └───────────────┼───────────────┘
                          │
                       Database
```

Anything the GUI can do should ultimately correspond to an operation in the core API.

This allows the exact same operations to be performed by:

* A user clicking in the editor
* A CLI command
* A Python script
* An MCP tool
* An AI agent
* An ACP chatbox
* A future plugin

This is particularly important for AI integration.

---

## 5. Technology Stack

Initial core technologies:

| Technology        | Purpose                                      |
| ----------------- | -------------------------------------------- |
| **C++**           | `icelib` core library and executables        |
| **Qt 6**          | `ice` GUI and desktop application            |
| **CLI11**         | `ice-cli` and relevant executable commands   |
| **reflect-cpp**   | Reflection/serialization/data representation |
| **Clipper2**      | 2D polygon Boolean and offset operations     |
| **ALIGN**         | Automated analog layout synthesis            |
| **ngspice**       | SPICE simulation                             |
| **Python**        | User-facing scripting and automation         |
| **nanobind**      | C++ to Python bindings                       |
| **gdstk**         | GDSII/OASIS layout interchange               |
| **Spatial index** | Fast geometry queries                        |
| **toml++**        | TOML parsing for packages and configuration  |
| **spdlog**        | Logging                                      |
| **xmake**         | Build system                                 |
| **agentsdk-cpp**  | MCP server and ACP chatbox integration       |
| **doctest**       | C++ unit testing                             |
| **Sphinx**        | Documentation                                |

---

## 6. Schematic Editor

The schematic editor should support:

* Hierarchical schematics
* Symbols
* Pins
* Nets
* Labels
* Wires
* Bus structures
* Instances
* Parameters
* Device properties
* PDK-specific devices
* Generic devices
* Reusable schematic libraries
* Cross-probing with layout
* Simulation/testbench integration
* ERC
* Netlist generation

Example hierarchy:

```text
Project
└── Schematic
    ├── top
    ├── opamp
    ├── differential_pair
    └── bias_generator
```

The schematic database should be independent from the graphical representation.

A schematic symbol should not simply be a collection of Qt graphics objects.

Instead:

```text
Schematic Database
        │
        ├── Cell
        ├── Instance
        ├── Pin
        ├── Net
        ├── Parameter
        └── Symbol
                │
                ▼
             Qt View
```

### Symbol format

Schematic symbols should be defined as SVG artwork plus a TOML sidecar.

The TOML references the SVG and owns all semantic data. The SVG owns only artwork.

```text
symbols/
├── nmos.svg
├── nmos.toml
└── nmos_lowvt.toml
```

Multiple TOML symbols may reference the same SVG file, so devices sharing artwork do not duplicate it:

```toml
[symbol]
name = "nmos_lowvt"
artwork = "nmos.svg"
# Symbol units, origin, and y-orientation are fixed by convention.

[[pin]]
name = "G"
x = 0
y = 20
direction = "input"

[[pin]]
name = "D"
x = 20
y = 40
direction = "inout"
```

At load, ICE should parse the TOML + SVG pair into the schematic database. The database is the runtime source of truth; the files are the authoring format. SVG files must contain no scripts or external references.

A validator should reject missing SVGs, undefined pins, and off-grid port positions.

---

## 7. IC Layout Editor

The layout editor is a first-class part of the same ICE executable.

It should support:

* Polygon editing
* Rectangle editing
* Paths
* Vias
* Labels
* Ports
* Instances
* Hierarchy
* Layer visibility
* Layer selection
* Snapping
* Grid
* Copy/paste
* Move
* Rotate
* Mirror
* Arrays
* Boolean operations
* Offset operations
* Connectivity-aware editing
* Net highlighting
* DRC feedback
* Cross-probing with schematics
* Python automation
* AI-assisted editing

The layout editor should be useful both for:

```text
manual layout
```

and:

```text
automatically generated layout → manual refinement
```

---

## 8. Layout Database

The layout database should be **ICE's own representation**.

It should be defined natively in ICE rather than mirroring an external tool's data model.

Conceptually:

```text
Design
├── Libraries
├── Cells
│   ├── Shapes
│   ├── Instances
│   ├── Ports
│   └── Properties
├── Nets
├── Layers
└── PDK
```

A shape can conceptually contain:

```text
shape
├── geometry
├── layer
├── net
└── properties
```

Coordinates should use **integer database units**, rather than floating-point coordinates.

For example:

```cpp
using coordinate_t = std::int64_t;
```

The database unit can represent a fixed physical resolution such as nanometers.

This provides deterministic geometry operations and avoids floating-point ambiguity in DRC and layout generation.

---

## 9. Geometry Kernel

ICE should have a custom EDA-oriented geometry API.

It should **not** expose Clipper2 types throughout the entire codebase.

Conceptually:

```cpp
class geometry_kernel {
public:
    polygon unite(...);
    polygon subtract(...);
    polygon intersect(...);
    polygon xor_(...);
    std::vector<polygon> offset(...);
};
```

Clipper2 implements the underlying computational geometry:

```text
ICE Geometry API
       │
       ▼
Geometry Kernel
       │
       ▼
Clipper2
```

Clipper2 is the implementation behind ICE's geometry model, and it covers the operations ICE needs:

* Union
* Intersection
* Difference
* XOR
* Offset
* Polygon cleanup
* Path processing

Those operations are required by:

* Layout editing
* DRC
* Enclosure calculations
* Spacing calculations
* Metal construction
* Via generation
* Automated routing
* PDK generators
* Device generation

This gives ICE ownership of its geometry model while avoiding the complexity and risk of implementing robust polygon clipping from scratch.

---

## 10. Spatial Index

The layout editor will eventually need to handle potentially very large numbers of shapes.

A spatial index should support queries such as:

```text
Which shapes are under the cursor?
Which shapes intersect this rectangle?
Which shapes are near this geometry?
Which objects need DRC re-evaluation?
Which objects overlap this route?
```

An R-tree spatial data structure should be used.

The spatial index should be integrated with the ICE layout database rather than becoming the layout database itself.

---

## 11. Hierarchical Layout

Hierarchy should be a core feature.

```text
Top Cell
├── Instance A
│   └── Cell A
├── Instance B
│   └── Cell B
└── Instance C
    └── Cell C
```

Instances should support transformations:

* Translation
* Rotation
* Mirroring
* Array replication

The layout system should preserve hierarchy when possible rather than flattening everything.

---

## 12. Ports

ICE should use a **port abstraction** for programmable layout.

A port should conceptually contain:

```text
Port
├── name
├── position
├── orientation
├── width
├── layer
└── properties
```

Ports enable generic operations such as:

```python
route(cell.port("input"), cell.port("output"))
```

without requiring the routing system to understand the implementation details of the cells.

This abstraction is especially useful for:

* PDK cells
* Analog blocks
* Standard cells
* Reusable generators
* Hierarchical layouts
* Routing
* Third-party layout libraries

---

## 13. PDK Architecture

PDKs should be treated as packages providing technology information and design functionality.

A PDK can provide:

```text
PDK
├── Layers
├── Layer rules
├── Design rules
├── Via definitions
├── Device definitions
├── Device generators
├── Symbols
├── Models
├── Extraction rules
├── DRC rules
├── LVS rules
└── Simulation information
```

The core ICE layout API should remain PDK-agnostic.

For example:

```python
cell.rectangle("M1", ...)
```

should work independently of the specific PDK.

The PDK defines what `M1` means.

Higher-level PDK functionality can provide:

```python
pdk.nmos(...)
pdk.pmos(...)
pdk.via(...)
```

---

## 14. Automated Analog Layout

ICE should integrate **ALIGN** as an analog layout synthesis backend.

ALIGN's role is:

```text
Schematic / SPICE
       │
       ▼
ALIGN
       │
       ▼
Automated layout
       │
       ▼
ICE Layout Database
```

ALIGN should be treated as a synthesis engine rather than the owner of ICE's final layout database.

The desired workflow is:

```text
Schematic
    │
    ▼
ALIGN synthesis
    │
    ▼
Initial layout
    │
    ▼
ICE Layout Editor
    │
    ├── manual modifications
    ├── Python modifications
    ├── AI modifications
    └── automated optimization
    │
    ▼
Final layout
```

ICE should avoid a workflow where the layout must be exported to GDS and re-imported merely to move between ALIGN and ICE.

ALIGN must sit behind a narrow, ICE-owned adapter boundary. The core should only know versioned `AnalogLayoutRequest` / `AnalogLayoutResult` structs built from PDK-agnostic nets, layers, devices, and constraints. The adapter owns all ALIGN specifics: input and constraint file formats, invocation as an out-of-process run with a pinned environment, output parsing, and PDK name/layer mappings. No ALIGN types, file schemas, or PDK-generator names may appear in core headers or the canonical layout database.

This keeps ALIGN and PDK churn from leaking into the core DB, and allows ALIGN to be pinned, mocked, or replaced without touching the core.

---

## 15. Generic Layout Scripting

ICE should provide a **Python-first layout scripting API**.

The scripting API operates directly on the ICE layout database:

```text
Python
  │
  ▼
ICE Python API
  │
  ▼
ICE Layout Database
  │
  ▼
Geometry Kernel
  │
  ▼
Clipper2
```

Python scripts should be able to create:

* Cells
* Shapes
* Paths
* Vias
* Ports
* Instances
* Arrays
* Routes
* Boolean geometry
* PDK primitives
* Hierarchical structures

Example:

```python
from ice import Cell

cell = Cell("example")

m1 = cell.layer("M1")

cell.add(m1.rectangle(0, 0, 1000, 200))
cell.add(m1.rectangle(800, 0, 2000, 200))

cell.route(
    cell.port("input"),
    cell.port("output")
)
```

The exact API is intentionally subject to future design.

---

## 16. Python as the Unified Automation Language

Python should be the primary user-facing scripting language for ICE.

It should cover:

```text
Layout
Simulation
Testbenches
Model analysis
Parameter sweeps
Benchmarking
Optimization
Data analysis
Visualization
Project automation
Package interaction
```

The C++ API should be the underlying implementation API, exposed to Python.

Conceptually:

```text
             Python
                │
              nanobind
                │
                ▼
           ICE C++ API
```

This avoids maintaining separate Python and C++ implementations.

---

## 17. Analog Simulation

ICE should integrate **ngspice** via shared-library mode (**libngspice**).

The simulation backend should load libngspice in-process and drive it through its C API (init callbacks, load netlist, run, halt). Callbacks should stream vectors and status back during the run for live plotting, early-stop on divergence, and cancellation. ngspice data callbacks fire off the GUI thread, so the adapter must marshal results to the Qt thread and serialize concurrent runs.

The simulation subsystem should provide a higher-level API over libngspice.

Conceptually:

```python
tb = Testbench("opamp")

tb.supply("VDD", 1.8)
tb.input("VIN", ...)
tb.load("OUT", 10e-12)

result = tb.run(
    analysis="tran",
    duration=10e-6,
)
```

The exact API is subject to future design.

The user should not need to manually construct raw SPICE files for common workflows.

Raw SPICE should remain available for advanced users.

---

## 18. Testbench System

Testbenches should be first-class ICE objects.

A testbench should be able to specify:

* Sources
* Supplies
* Loads
* Initial conditions
* Temperature
* Corners
* Simulation analysis
* Measurements
* Output signals
* Parameters
* Sweeps

Supported analysis should eventually include:

```text
DC
AC
Transient
Noise
Operating point
Parameter sweeps
Temperature sweeps
Corner analysis
Monte Carlo, where supported
```

---

## 19. Model Analysis

ICE should provide tools for analyzing device and circuit models.

Examples:

```text
Transistor model analysis
Parameter extraction
Model comparison
Temperature behavior
Corner behavior
Frequency response
Noise behavior
DC characteristics
Small-signal characteristics
```

Python should be the main interface for advanced analysis.

Scientific Python libraries can be used for:

* Numerical analysis
* Optimization
* Data processing
* Plotting
* Statistical analysis
* Machine learning where useful

ICE should not duplicate functionality that is already well-covered by the Python scientific ecosystem.

---

## 20. Experiments

A reusable experiment abstraction should sit above individual simulations.

Conceptually:

```text
Model
  ↓
Circuit
  ↓
Testbench
  ↓
Experiment
  ↓
Results
```

For example:

```python
experiment = Sweep(testbench)

experiment.parameter(
    "temperature",
    [-40, 25, 85]
)

experiment.parameter(
    "vdd",
    [1.6, 1.8, 2.0]
)

results = experiment.run()
```

Results should be machine-readable and suitable for further analysis.

---

## 21. Analog Benchmarking

ICE should provide a benchmarking framework for analog circuits.

A benchmark should define:

* Circuit/design
* Testbench
* Metrics
* Constraints
* Simulation conditions
* Reference targets

Example:

```text
Opamp Benchmark
├── DC gain
├── Unity-gain bandwidth
├── Phase margin
├── Slew rate
├── Input-referred noise
├── Power consumption
└── Area
```

Benchmark results should support comparison across:

* Designs
* PDKs
* Process corners
* Layout versions
* Simulation configurations
* Optimization iterations

---

## 22. Layout-Aware Analog Optimization

A major long-term feature should be a closed design loop:

```text
Schematic
    │
    ▼
ALIGN
    │
    ▼
Layout
    │
    ▼
DRC
    │
    ▼
PEX
    │
    ▼
Post-layout SPICE
    │
    ▼
Benchmark
    │
    ▼
Optimization
    │
    └───────────────► improved design/layout
```

This allows ICE to evaluate designs based not only on schematic-level performance but also:

* Layout area
* Parasitics
* Routing
* Matching
* Post-layout performance
* Power
* Yield-related metrics

---

## 23. GDSII / OASIS

ICE should support standard layout interchange formats.

At minimum:

```text
GDSII
OASIS
```

The layout database should remain independent of the interchange representation.

```text
ICE Layout DB
      │
      ├── GDSII exporter
      ├── GDSII importer
      ├── OASIS exporter
      └── OASIS importer
```

The external format should never become the internal database representation.

---

## 24. Package Manager

ICE should include a first-class package manager.

The package manager should support installation of:

* PDKs
* PDK versions
* Symbol libraries
* Schematic libraries
* Layout libraries
* Device generators
* Python packages/scripts
* Simulation models
* Testbenches
* Benchmarks
* Plugins
* Themes/assets
* Documentation
* Other reusable design resources

Example commands:

```text
ice install sky130
ice install ihp-sg13g2
ice install analoglib
ice install opamp-library

ice update
ice remove analoglib
ice list
ice search opamp
```

---

## 25. ICE Package Format

A package should be simple and extensible.

Example:

```text
my-package/
├── package.toml
├── pdk/
├── symbols/
├── schematics/
├── layout/
├── models/
├── scripts/
├── plugins/
├── benchmarks/
└── docs/
```

Package metadata could use TOML:

```toml
[package]
name = "example.sky130"
version = "1.2.0"
description = "Example PDK package"
license = "..."

[dependencies]
ice = ">=0.1"
some-library = "^2.1"

[provides]
pdk = true
symbols = true
models = true
layout = true
python = true
```

The package system should be capability-oriented rather than forcing every package into a single rigid category.

---

## 26. Package Repositories

ICE should support multiple package repositories.

Example:

```text
ICE Official Repository
├── PDKs
├── official libraries
└── official plugins

Community Repository
├── symbols
├── generators
├── libraries
└── plugins

Private Repository
├── proprietary PDKs
├── company libraries
└── internal tools
```

Packages should have:

* Name
* Version
* Dependencies
* Metadata
* License
* Repository
* Checksums/signatures where appropriate

The package system should support reproducible version selection.

---

## 27. CLI

ICE should include a CLI implemented using **CLI11**.

The CLI should expose the complete set of **user-relevant core operations**
available through the editor. It should not be a reduced utility interface for
project mutations, simulation, verification, package management, or other
domain behavior.

The goal is:

```text
GUI action
     │
     ▼
icelib core operation
     ▲
     │
CLI command
```

For example:

```text
ice project create
ice project open
ice schematic open
ice schematic add-device
ice schematic connect
ice layout open
ice layout add-polygon
ice layout move
ice layout route
ice layout boolean
ice layout export
ice simulate
ice testbench run
ice benchmark run
ice drc
ice lvs
ice pex
ice package install
ice package search
```

The exact command hierarchy is subject to future design.

---

## 28. Core-operation parity

A fundamental ICE requirement is:

> **Every supported core operation should have a clearly defined interface
> contract and be available through the appropriate automation surfaces.**

Parity is defined at the **core-operation level**, not at the level of Qt
widgets or individual GUI gestures. The GUI, CLI, Python API, MCP server, and
plugins should invoke the same validated application operations rather than
implementing parallel domain logic.

Core operations include, where supported:

* Creating and editing project objects
* Connecting schematic nets
* Creating and modifying layout geometry
* Running simulations and experiments
* Running DRC, LVS, and PEX
* Generating and refining layouts
* Installing and resolving packages
* Running benchmarks
* Importing and exporting supported formats
* Running approved scripts
* Managing project state and provenance

The following are presentation or interaction concerns and do not require
standalone CLI equivalents:

* Viewport pan, zoom, and scroll position
* Hover feedback and preselection
* Qt dock layout and panel arrangement
* Cursor shape and visual affordances
* Interactive previews before a core operation is committed
* Modal dialogs, provided their underlying operation is exposed
* Temporary selection or focus state, unless it is required as an explicit
  input to a core operation

Interactive GUI behavior should still produce or preview the same core command
that automation would invoke. A GUI-only interaction is acceptable when it is
purely presentational; it is not acceptable when it hides domain behavior that
cannot be reproduced or scripted.

## Capability matrix

ICE should maintain a capability matrix for every public core operation. The
matrix should identify whether an operation is:

* supported;
* read-only;
* mutation-capable;
* previewable;
* asynchronous;
* undoable;
* permission-gated;
* available through each interface.

The matrix should be kept with the interface documentation and updated when a
new core operation or adapter is added.

| Capability area | GUI | CLI | Python | MCP | Plugin |
| --- | --- | --- | --- | --- | --- |
| Create/edit project objects | Yes | Yes | Yes | Yes | Contract-dependent |
| Inspect project state | Yes | Yes | Yes | Yes | Contract-dependent |
| Run simulations/jobs | Yes | Yes | Yes | Yes | Backend-dependent |
| Preview mutations | Yes | Yes | Yes | Yes | Contract-dependent |
| Undo/redo committed mutations | Yes | Yes, where session-backed | Yes | Yes, transaction-backed | Contract-dependent |
| Viewport and presentation state | Yes | No requirement | No requirement | No requirement | No requirement |
| Package and PDK operations | Yes | Yes | Yes | Permission-gated | Permission-gated |
| Verification and extraction | Yes | Yes | Yes | Yes | Backend-dependent |

“Yes” means the interface exposes the operation through the shared application
contract; it does not mean that every interface has identical syntax or user
interaction. “Contract-dependent” and “Backend-dependent” require the relevant
capability to be declared explicitly rather than silently omitted.

---

## 29. `ice-mcp` Server

ICE should provide an **`ice-mcp` executable** for AI agents, implemented with
**agentsdk-cpp** (`agentsdk::mcp::mcp_server`,
https://github.com/ldapx/agentsdk-cpp) over stdio JSON-RPC.

The `ice-mcp` executable should expose the same `icelib` core functionality as
`ice` and `ice-cli`.

Potential tool categories:

```text
Project
Schematic
Layout
Simulation
Analysis
Benchmark
PDK
Package Manager
Python
DRC
LVS
PEX
```

For example, an AI agent could eventually perform operations conceptually equivalent to:

```text
create_cell
add_instance
connect_net
create_layout
place_instance
move_shape
route_net
run_drc
run_simulation
run_benchmark
install_package
```

The MCP layer should call the same `icelib` APIs used by `ice` and `ice-cli`.

It should **not** implement its own layout manipulation logic.

Each ICE operation should map to an agentsdk-cpp tool handler (`tools/list` with pagination, `tools/call` with `isError` results). `resources/list` / `resources/read`, `prompts/list` / `prompts/get`, capability negotiation via the `initialize` handshake, and progress/cancellation notifications should be used where they fit instead of inventing new mechanisms.

---

## 30. Agent Client Protocol Chatbox

ICE should also contain an **Agent Client Protocol chatbox**, implemented with **agentsdk-cpp** (`agentsdk::acp::acp_client`, `acp_session`, `acp_agent_manager`).

The chatbox is a user-facing AI interface inside ICE. It should drive a local coding agent as a subprocess over stdio JSON-RPC, discover agents on `PATH`, manage session state, and surface tool-call updates and permission prompts inside the ICE chat panel.

Conceptually:

```text
┌─────────────────────────────────────────────┐
│ ICE                                         │
│                                             │
│  Schematic / Layout / Simulation            │
│                                             │
│                                  ┌────────┐ │
│                                  │ Chat   │ │
│                                  │        │ │
│                                  │ User:  │ │
│                                  │ ...    │ │
│                                  │        │ │
│                                  └────────┘ │
└─────────────────────────────────────────────┘
```

The agent should be able to operate on the currently opened project.

For example:

```text
User:
"Create a differential pair and place it near the bias circuit."

Agent:
→ inspect schematic
→ create devices
→ connect nets
→ generate layout
→ place layout
→ report changes
```

The chatbox should not have a separate private representation of the project.

It should operate through the same `icelib` APIs exposed to `ice-cli` and
`ice-mcp`.

---

## 31. AI Architecture

The desired architecture is:

```text
                          icelib
                            │
           ┌────────────────┼────────────────┐
           │                │                │
          GUI              CLI            Python
           │                │                │
           └────────────────┼────────────────┘
                            │
                     Core Operations
                            │
             ┌──────────────┴──────────────┐
             │                             │
        ice-mcp                  ACP Chatbox in ice
             │                             │
          External                   User-facing
          AI agents                     agent
```

This prevents AI functionality from becoming tightly coupled to the GUI.

---

## 32. Reflection and Serialization

**reflect-cpp** should be used where appropriate for:

* Reflection
* Serialization
* Project data
* Configuration
* Metadata
* Structured interchange
* Potential RPC/API representations

However, the serialized representation should remain an ICE-defined format rather than exposing implementation-specific third-party types.

---

## 33. Project Files

An ICE project is a directory with one JSON project file and per-document JSON
files:

```text
project/
├── project.icepro       # project metadata only
├── sch/
│   └── <cell>.icesch    # one schematic cell per file
├── lay/
│   └── <cell>.icelay    # one layout cell or procedural layout per file
├── sim/
│   └── <sim>.icesim     # one simulation per file
├── reports/
│   └── <report>.py      # project-level simulation report
├── build/               # generated simulation runs, ignored by Git
├── .gitignore           # ignores build/
└── AGENTS.md
```

The `.icepro` file contains identity, name, units, grid, package
dependencies, layer metadata, and project-level report associations. It does
not contain schematic, layout, or simulation objects. Report scripts are
project source files; generated run data goes under `build/` and is ignored by
Git.

A lockfile should eventually record exact package/PDK versions used by the project.

For example:

```text
project
    │
    ├── ICE version
    ├── PDK version
    ├── library versions
    ├── plugin versions
    └── Python/package environment
```

This is important for reproducible simulation and layout results.

While ICE is in development with no users, file-format migrations and backwards compatibility are explicitly non-goals. Formats may break between versions without migration paths; version stamps should be recorded so breakage is detectable, not so old files remain loadable.

---

## 34. DRC

DRC should eventually be integrated into the layout environment.

The system should support:

* Minimum width
* Minimum spacing
* Enclosure
* Extension
* Minimum area
* Layer interactions
* Via rules
* Density rules
* Other PDK-specific rules

The DRC system should be able to provide immediate/local feedback during editing as well as full-project checks.

Conceptually:

```text
User edits geometry
       │
       ▼
Spatial index
       │
       ▼
Affected geometry
       │
       ▼
Incremental DRC
       │
       ▼
Editor markers
```

---

## 35. LVS

LVS should compare:

```text
Schematic
    ↕
Extracted Layout
```

The LVS system should eventually identify:

* Missing devices
* Extra devices
* Incorrect connections
* Incorrect device parameters
* Incorrect hierarchy
* Incorrect multiplicity
* Other topology mismatches

---

## 36. Parasitic Extraction

PEX should bridge layout and post-layout simulation:

```text
Layout
  │
  ▼
Extraction
  │
  ▼
Parasitic model
  │
  ▼
SPICE
  │
  ▼
Post-layout results
```

The extracted representation should be usable directly by the ICE simulation/benchmarking environment.

---

## 37. Manual + Automated Design

A central ICE philosophy should be that **automation and manual editing are not separate workflows**.

Instead:

```text
Automatic
    │
    ▼
Generated layout
    │
    ▼
Manual editing
    │
    ▼
Python automation
    │
    ▼
AI editing
    │
    ▼
Optimization
    │
    ▼
Final layout
```

All operations manipulate the same layout database.

---

## 38. Extensibility

ICE should support experimental third-party plugins. During the pre-user
development phase, plugin APIs are allowed to change or break without
compatibility shims or migration paths. Plugin authors should target a specific
ICE build or source revision, and package metadata should record that target so
incompatibilities are detectable.

Potential plugin capabilities:

```text
GUI panels
Editors
PDK support
Generators
Simulation backends
Analysis tools
Import/export formats
AI tools
Package types
Benchmark suites
```

## Experimental plugin policy

Plugin stability is intentionally deferred. Until a future release explicitly
introduces a stable plugin API:

- No API or ABI compatibility guarantee is made between ICE builds.
- Plugins must declare the ICE build, source revision, or development API
  snapshot they target.
- `ice-pkg` should reject or warn about plugins whose declared target does not
  match the installed ICE version.
- Plugin lifecycle hooks should still be explicit: discover, load, initialize,
  enable, disable, and unload where technically possible.
- Plugin dependencies and required capabilities should be declared in package
  metadata, even though the metadata schema may evolve.
- GUI/editor plugins may run in-process while the API is experimental.
- Plugins that execute untrusted code, invoke external tools, or can affect
  project integrity should prefer an out-of-process worker boundary.
- Plugin failures must be reported and must not silently corrupt the project
  database.
- Plugin operations must use `icelib` services rather than maintaining a
  competing project representation.

API/ABI versioning, long-term capability negotiation, compatibility windows,
deprecation policy, and migration tooling are deferred until the plugin
ecosystem is large enough to justify them. This deferral does not remove the
need for explicit lifecycle, dependency, permission, and failure behavior.

---

## 39. Separation of Concerns

The architecture should maintain strict boundaries:

```text
GUI
 │
 ▼
ICE Application API
 │
 ▼
Domain Model
 │
 ├── Schematic
 ├── Layout
 ├── Simulation
 ├── PDK
 ├── Packages
 └── Analysis
 │
 ▼
Backend adapters
 │
 ├── Clipper2
 ├── ALIGN
 ├── ngspice
 ├── GDS/OASIS
 └── other tools
```

External dependencies should generally be accessed through adapters.

For example:

```text
ICE Layout Geometry
        │
        ▼
Geometry Kernel Interface
        │
        ▼
Clipper2 Backend
```

rather than spreading Clipper2 types throughout ICE.

Likewise:

```text
ICE Simulation API
        │
        ▼
ngspice Adapter
        │
        ▼
ngspice
```

and:

```text
ICE Analog Layout API
        │
        ▼
ALIGN Adapter
        │
        ▼
ALIGN
```

---

## 40. Dependencies

The core should remain relatively small.

### Core dependencies

```text
C++
Qt 6
CLI11
reflect-cpp
Clipper2
agentsdk-cpp (MCP server + ACP client; pulls in simdjson, libcurl, spdlog)
toml++ (TOML parsing)
spdlog (logging)
nanobind (C++ to Python bindings)
```

### Domain backends

```text
ngspice
ALIGN
gdstk (GDSII/OASIS layout interchange)
Spatial index
```

### User-facing ecosystem

```text
Python
NumPy
SciPy
pandas/Polars
Matplotlib
Jupyter
```

The user-facing ecosystem libraries are optional and should not become hard dependencies of the ICE executable.

### Developer tooling

```text
xmake (build system)
doctest (C++ unit tests)
Sphinx (documentation)
```

These run at build/docs time only and should not become hard dependencies of the ICE executable.

### Code quality and style

ICE must be solid as rock. Production code should favor correctness,
determinism, explicit error handling, strong type safety, and clear ownership
over shortcuts or fragile convenience abstractions. C++ and Qt code must follow
the [Qt Coding Style](https://wiki.qt.io/Qt_Coding_Style); reviews and automated
format/style checks should enforce the project conventions as they are added.

---

## 41. Unified Workflow

A complete ICE analog workflow should eventually look like:

```text
                    PDK
                     │
                     ▼
                Schematic
                     │
                     ▼
              Device selection
                     │
                     ▼
                Testbench
                     │
                     ▼
                 ngspice
                     │
                     ▼
               Model analysis
                     │
                     ▼
                Benchmark
                     │
                     ▼
                 ALIGN
                     │
                     ▼
             Initial layout
                     │
                     ▼
              ICE Layout Editor
                     │
           ┌─────────┼─────────┐
           │         │         │
        Manual     Python      AI
         edits     scripts    agent
           │         │         │
           └─────────┼─────────┘
                     ▼
                    DRC
                     │
                     ▼
                    LVS
                     │
                     ▼
                    PEX
                     │
                     ▼
             Post-layout SPICE
                     │
                     ▼
                 Benchmark
                     │
                     ▼
                Optimization
```

---

## 42. AI-Assisted Workflow

The AI layer should be capable of operating at multiple levels.

### High-level

```text
"Generate an op-amp using this topology."
```

### Schematic

```text
"Add a current mirror to the bias circuit."
```

### Layout

```text
"Move the differential pair closer together."
```

### Geometry

```text
"Extend this metal route to the via."
```

### Analysis

```text
"Find why the phase margin dropped after layout."
```

### Optimization

```text
"Optimize this design for gain and power while keeping the area below the target."
```

The agent should use ICE's structured APIs rather than interacting with Qt widgets as if it were a human.

---

## 43. Long-Term Vision

ICE should eventually function as a programmable IC design environment:

```text
                     ┌───────────────┐
                     │      ICE      │
                     └───────┬───────┘
                             │
          ┌──────────────────┼──────────────────┐
          │                  │                  │
       Schematic           Layout           Simulation
          │                  │                  │
          └──────────────────┼──────────────────┘
                             │
                         Analysis
                             │
                         Benchmark
                             │
                       Optimization
                             │
                    ┌────────┴────────┐
                    │                 │
                 Python               AI
                    │                 │
                    └────────┬────────┘
                             │
                         Automation
```

The most important architectural principle is:

> **ICE should have one canonical design model and one set of core operations,
> with `ice`, `ice-cli`, `ice-pkg`, `ice-mcp`, Python bindings, and the agent
> chatbox all acting as different interfaces to `icelib`.**

That allows ICE to be simultaneously:

* A traditional schematic/layout editor
* A programmable layout environment
* An analog simulation environment
* An automated layout system
* A benchmarking/optimization framework
* A package ecosystem
* An AI-native EDA environment

without requiring separate implementations of the same functionality.
