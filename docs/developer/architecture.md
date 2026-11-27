# OpenVLSI Architecture

> Product intent is in {doc}`/project/vision`; capabilities and acceptance
> criteria are in {doc}`/project/roadmap`.

## Overview

OpenVLSI is an open-source, extensible **integrated-circuit design environment** focused on schematic design, IC layout, analog simulation, automated layout, analysis, benchmarking, and programmable design workflows.

OpenVLSI is a unified product composed of several executables and one shared core
library. The executables provide focused process boundaries while reusing the
same domain model and application services from `openvlsilib`.

```text
                         OpenVLSI product
                              │
              ┌───────────────┼───────────────┐
              │               │               │
             openvlsi           openvlsi-cli         openvlsi-pkg
        main EDA GUI     automation CLI   package manager
              │               │               │
              └───────────────┼───────────────┘
                              │
                           openvlsilib
                    canonical core library
                              │
                           openvlsi-mcp
                         MCP server
```

The executables, Python bindings, MCP server, and plugins must operate on the
same `openvlsilib` project/database model rather than implementing independent
domain logic.

---

## 1. Goals

OpenVLSI should provide an integrated workflow for:

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
14. Automating OpenVLSI through Python.
15. Allowing AI agents to operate OpenVLSI through MCP.
16. Providing an Agent Client Protocol chat interface inside OpenVLSI.
17. Supporting third-party extensions without making them part of the OpenVLSI core.

---

## 2. High-Level Architecture

```text
                                      OpenVLSI
                                       │
             ┌─────────────────────────┼─────────────────────────┐
             │                         │                         │
             ▼                         ▼                         ▼
            openvlsi                     openvlsi-cli                    Python
         Qt 6 GUI                CLI11 frontend              bindings
             │                         │                         │
             └─────────────────────────┼─────────────────────────┘
                                       │
                                openvlsilib Core API
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
                         openvlsilib
                            │
                ┌───────────┴───────────┐
                │                       │
             openvlsi-mcp                 openvlsi-pkg
          MCP JSON-RPC          package/PDK manager
                │
            AI agents
```

---

## 3. Product Executables and Process Boundaries

OpenVLSI should not be one executable. It should consist of the following primary
executables:

| Executable | Responsibility |
| --- | --- |
| `openvlsi` | Main Qt desktop EDA application: schematic, layout, simulation, analysis, and integrated project workflows. |
| `openvlsi-cli` | Headless automation and scripting entry point for core operations. |
| `openvlsi-pkg` | Package and PDK installation, resolution, verification, repository, and lockfile management. |
| `openvlsi-mcp` | MCP server exposing approved `openvlsilib` operations to external agents. |

All four executables link to or otherwise use `openvlsilib`. They must not maintain
separate domain models. Python bindings should also be generated from
`openvlsilib`, and the `openvlsi` application may host the in-process ACP chatbox.

The main `openvlsi` application contains:

```text
OpenVLSI
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

`openvlsi-pkg` remains a separate executable because package installation and
repository operations have different permissions, lifecycles, and failure
boundaries from interactive editing. `openvlsi-mcp` remains separate so an external
agent connection cannot require the GUI process.

The schematic and layout editors, `openvlsi-cli`, `openvlsi-pkg`, and `openvlsi-mcp` should
share through `openvlsilib`:

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
                           │ openvlsi-mcp │
                           └────┬────┘
                                │
                                ▼
┌──────────────┐      ┌──────────────────────┐      ┌──────────────┐
│     openvlsi      │      │       openvlsilib         │      │   openvlsi-cli    │
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
                 │  openvlsi-pkg │
                 │ packages │
                 └────┬─────┘
                      │
                      ▼
             repositories, PDKs, lockfiles

Optional workers are separate processes where isolation or tool lifecycle
requires it:

  openvlsi / openvlsi-cli / openvlsi-mcp
              │
              ├── ngspice worker
              ├── ALIGN worker
              ├── DRC/LVS/PEX workers
              └── plugin or package worker
```

`openvlsilib` owns the canonical model and operation contracts. Workers communicate
through versioned adapter protocols and return normalized results. A worker
failure must not corrupt the caller's project state.

---

## 4. Core Design Principle

The GUI should **not** be the primary abstraction.

`openvlsilib` should provide the reusable application API:

```text
                       openvlsilib API
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
| **C++**           | `openvlsilib` core library and executables        |
| **Qt 6**          | `openvlsi` GUI and desktop application            |
| **CLI11**         | `openvlsi-cli` and relevant executable commands   |
| **reflect-cpp**   | Reflection/serialization/data representation |
| **Clipper2**      | 2D polygon Boolean and offset operations     |
| **ALIGN**         | Automated analog layout synthesis            |
| **ngspice**       | SPICE simulation                             |
| **Python**        | User-facing scripting and automation         |
| **nanobind**      | C++ to Python bindings                       |
| **zlib**          | OASIS `CBLOCK` decompression                 |
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

At load, OpenVLSI should parse the TOML + SVG pair into the schematic database. The database is the runtime source of truth; the files are the authoring format. SVG files must contain no scripts or external references.

A validator should reject missing SVGs, undefined pins, and off-grid port positions.

---

## 7. IC Layout Editor

The layout editor is a first-class part of the same OpenVLSI executable.

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

The layout database should be **OpenVLSI's own representation**.

It should be defined natively in OpenVLSI rather than mirroring an external tool's data model.

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

OpenVLSI calls a layout-cell terminal a **port** and a schematic-symbol terminal a
**pin**. LEF, DEF, and GDS use `PIN`/pin-label vocabulary as file-format
keywords. Ports are intrinsic OpenVLSI annotations. They carry a name, rectangle,
direction, side, net, and label, and do not require a process-layer
association: the layer is required, persisted with the port, and drives
routing, DRC/LVS matching, and interchange. A PDK pin-purpose or label layer is
only an optional interchange projection, never the source of truth for port
identity. GDS/OASIS writers project each port onto its layer; LEF/DEF writers
use the same layer. `OPENVLSI_PORT` remains a fallback for malformed interchange
input with no layer.

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

OpenVLSI should have a custom EDA-oriented geometry API.

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
OpenVLSI Geometry API
       │
       ▼
Geometry Kernel
       │
       ▼
Clipper2
```

Clipper2 is the implementation behind OpenVLSI's geometry model, and it covers the operations OpenVLSI needs:

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

This gives OpenVLSI ownership of its geometry model while avoiding the complexity and risk of implementing robust polygon clipping from scratch.

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

The spatial index should be integrated with the OpenVLSI layout database rather than becoming the layout database itself.

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

OpenVLSI should use a **port abstraction** for programmable layout.

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

The core OpenVLSI layout API should remain PDK-agnostic.

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

OpenVLSI should integrate **ALIGN** as an analog layout synthesis backend.

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
OpenVLSI Layout Database
```

ALIGN should be treated as a synthesis engine rather than the owner of OpenVLSI's final layout database.

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
OpenVLSI Layout Editor
    │
    ├── manual modifications
    ├── Python modifications
    ├── AI modifications
    └── automated optimization
    │
    ▼
Final layout
```

OpenVLSI should avoid a workflow where the layout must be exported to GDS and re-imported merely to move between ALIGN and OpenVLSI.

ALIGN must sit behind a narrow, OpenVLSI-owned adapter boundary. The core should only know versioned `AnalogLayoutRequest` / `AnalogLayoutResult` structs built from PDK-agnostic nets, layers, devices, and constraints. The adapter owns all ALIGN specifics: input and constraint file formats, invocation as an out-of-process run with a pinned environment, output parsing, and PDK name/layer mappings. No ALIGN types, file schemas, or PDK-generator names may appear in core headers or the canonical layout database.

This keeps ALIGN and PDK churn from leaking into the core DB, and allows ALIGN to be pinned, mocked, or replaced without touching the core.

---

## 15. Generic Layout Scripting

OpenVLSI should provide a **Python-first layout scripting API**.

The scripting API operates directly on the OpenVLSI layout database:

```text
Python
  │
  ▼
OpenVLSI Python API
  │
  ▼
OpenVLSI Layout Database
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
from openvlsi import Cell

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

Python should be the primary user-facing scripting language for OpenVLSI.

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
           OpenVLSI C++ API
```

This avoids maintaining separate Python and C++ implementations.

---

## 17. Analog Simulation

OpenVLSI should integrate **ngspice** via shared-library mode (**libngspice**).

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

Testbenches should be first-class OpenVLSI objects.

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

The ngspice adapter currently supports:

```text
DC sweep
AC sweep
Transient
Noise
Operating point
Transfer function
Parameter sweeps
Temperature sweeps
Corner analysis
Monte Carlo sampling
```

`SimulationItem` stores the analysis-specific fields, corners, sweeps, Monte
Carlo options, and declarative measurements. The GUI experiment dialog, CLI
(`simulation-configure`, `simulation-corner-*`, `simulation-sweep-*`,
`simulation-monte-carlo`, `simulation-measure-*`, `simulation-experiment`), and
MCP tools (`simulation_update`, `simulation_corner_defaults`,
`simulation_experiment`) all use the same `Experiment`/`SimulationRunner`
contract.

Waveform analysis is presentation-only and lives in `openvlsilib/waveform.*`.
`computeSpectrum` resamples onto a uniform grid and returns amplitude/phase
spectra; `evaluateDerivedTrace` evaluates safe arithmetic/function expressions
over captured traces; `buildWaveformSeries` applies a graph's view, derived
traces, and optional FFT. The Qt plot consumes those series but never mutates
stored runs. Graph-level analysis settings, stored-run overlay ids, and point
markers are serialized on `SimulationGraph` so the GUI, CLI, and MCP agree.

---

## 19. Model Analysis

OpenVLSI should provide tools for analyzing device and circuit models.

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

OpenVLSI should not duplicate functionality that is already well-covered by the Python scientific ecosystem.

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

OpenVLSI should provide a benchmarking framework for analog circuits.

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

This allows OpenVLSI to evaluate designs based not only on schematic-level performance but also:

* Layout area
* Parasitics
* Routing
* Matching
* Post-layout performance
* Power
* Yield-related metrics

---

## 23. GDSII / OASIS

OpenVLSI should support standard layout interchange formats.

At minimum:

```text
GDSII
OASIS
```

The layout database should remain independent of the interchange representation.

```text
OpenVLSI Layout DB
      │
      ├── GDSII exporter
      ├── GDSII importer
      ├── OASIS exporter
      └── OASIS importer
```

The external format should never become the internal database representation.

The current GDSII adapter reads structures as layout cells, including
`SREF`/`AREF` placement, `BOUNDARY`, `PATH`, `BOX`, `TEXT`, and units. Orthogonal
references become layout instances; arrays are expanded into instances.
Non-orthogonal or scaled references are flattened into their parent cell with a
warning because the canonical layout instance transform is limited to the eight
orthogonal/mirror orientations. The writer preserves reachable cell hierarchy
by default and emits `SREF` records; a flattened single-structure export remains
available. OASIS 1.0 import/export is implemented natively against the same
internal model: the reader handles cell-name tables, modal placement and
geometry state, repetitions, properties, trapezoids, circles, and zlib
`CBLOCK` compression; the writer emits uncompressed hierarchical OASIS with a
cell-name table. Neither path depends on KLayout or a GDS intermediate file.

---

## 24. Package Manager

OpenVLSI should include a first-class package manager.

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
openvlsi install sky130
openvlsi install ihp-sg13g2
openvlsi install analoglib
openvlsi install opamp-library

openvlsi update
openvlsi remove analoglib
openvlsi list
openvlsi search opamp
```

---

## 25. OpenVLSI Package Format

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
openvlsi = ">=0.1"
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

OpenVLSI should support multiple package repositories.

Example:

```text
OpenVLSI Official Repository
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

OpenVLSI should include a CLI implemented using **CLI11**.

The CLI should expose the complete set of **user-relevant core operations**
available through the editor. It should not be a reduced utility interface for
project mutations, simulation, verification, package management, or other
domain behavior.

The goal is:

```text
GUI action
     │
     ▼
openvlsilib core operation
     ▲
     │
CLI command
```

For example:

```text
openvlsi project create
openvlsi project open
openvlsi schematic open
openvlsi schematic add-device
openvlsi schematic connect
openvlsi layout open
openvlsi layout add-polygon
openvlsi layout move
openvlsi layout route
openvlsi layout boolean
openvlsi layout export
openvlsi simulate
openvlsi testbench run
openvlsi benchmark run
openvlsi drc
openvlsi lvs
openvlsi pex
openvlsi package install
openvlsi package search
```

The exact command hierarchy is subject to future design.

---

## 28. Core-operation parity

A fundamental OpenVLSI requirement is:

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

OpenVLSI should maintain a capability matrix for every public core operation. The
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

## 29. `openvlsi-mcp` Server

OpenVLSI should provide an **`openvlsi-mcp` executable** for AI agents, implemented with
**agentsdk-cpp** (`agentsdk::mcp::mcp_server`,
https://github.com/ldapx/agentsdk-cpp) over stdio JSON-RPC.

The `openvlsi-mcp` executable should expose the same `openvlsilib` core functionality as
`openvlsi` and `openvlsi-cli`.

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

The MCP layer should call the same `openvlsilib` APIs used by `openvlsi` and `openvlsi-cli`.

It should **not** implement its own layout manipulation logic.

Each OpenVLSI operation should map to an agentsdk-cpp tool handler (`tools/list` with pagination, `tools/call` with `isError` results). `resources/list` / `resources/read`, `prompts/list` / `prompts/get`, capability negotiation via the `initialize` handshake, and progress/cancellation notifications should be used where they fit instead of inventing new mechanisms.

---

## 30. Agent Client Protocol Chatbox

OpenVLSI should also contain an **Agent Client Protocol chatbox**, implemented with **agentsdk-cpp** (`agentsdk::acp::acp_client`, `acp_session`, `acp_agent_manager`).

The chatbox is a user-facing AI interface inside OpenVLSI. It should drive a local coding agent as a subprocess over stdio JSON-RPC, discover agents on `PATH`, manage session state, and surface tool-call updates and permission prompts inside the OpenVLSI chat panel.

Conceptually:

```text
┌─────────────────────────────────────────────┐
│ OpenVLSI                                         │
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
→ summarize changes
```

The chatbox should not have a separate private representation of the project.

It should operate through the same `openvlsilib` APIs exposed to `openvlsi-cli` and
`openvlsi-mcp`.

---

## 31. AI Architecture

The desired architecture is:

```text
                          openvlsilib
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
        openvlsi-mcp                  ACP Chatbox in openvlsi
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

However, the serialized representation should remain an OpenVLSI-defined format rather than exposing implementation-specific third-party types.

---

## 33. Project Files

An OpenVLSI project is a directory with one JSON project file and per-document JSON
files:

```text
project/
├── project.ovpro       # project metadata only
├── sch/
│   └── <cell>.ovsch    # one schematic cell per file
├── lay/
│   └── <cell>.ovlay    # one layout cell or procedural layout per file
├── impl/
│   └── <run>.ovimpl    # one RTL-to-GDS implementation run per file
├── sim/
│   └── <sim>.ovsim     # one simulation per file
├── scripts/
│   └── <script>.py      # project-level simulation script
├── build/               # generated simulation/implementation artifacts, ignored by Git
├── .gitignore           # ignores build/
└── AGENTS.md
```

The `.ovpro` file contains identity, name, units, grid, package
dependencies, layer metadata, project-level simulation script associations, and
an inventory of the sidecar files that belong to the project. It does not
contain the sidecar objects themselves. The inventory lets `Project::load`
detect a missing cell file instead of silently opening a smaller project, and
detect or quarantine stale files left by an interrupted save. Simulation
scripts are project source files; generated simulation and implementation
artifacts go under `build/` and are ignored by Git.

A lockfile should eventually record exact package/PDK versions used by the project.

For example:

```text
project
    │
    ├── OpenVLSI version
    ├── PDK version
    ├── library versions
    ├── plugin versions
    └── Python/package environment
```

This is important for reproducible simulation and layout results.

While OpenVLSI is in development with no users, file-format migrations and backwards compatibility are explicitly non-goals. Formats may break between versions without migration paths; version stamps should be recorded so breakage is detectable, not so old files remain loadable.

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

The current comparator accepts drawn schematics, parsed text SPICE netlists
(including recursively flattened `.subckt` instances and resolved
`.include`/`.lib` files), and structural SystemVerilog cells. Text SPICE device
identity, parameters, and terminal nets feed the same device/net comparison
engine as drawn schematics. Layout connectivity combines planned connections
and terminal metadata with geometry-derived nets from shapes, routed paths,
vias, and placed-device port overlaps; shorts through unlabeled geometry are
reported explicitly. Device matching uses schematic links and names first, then
a topology fallback after `m=` multiplicity expansion. Behavioral
SystemVerilog is rejected for LVS instead of being compared as ports only;
gate-level module instances and primitives are compared device by device.

LVS also returns the complete correspondence, not only failures. Device matches
record whether they came from an authored link, a unique normalized name, or a
terminal-topology match; pin and net maps, parameter comparisons, typed object
references, and layout bounds are included. Authored links remain stored on
layout instances and planned devices, so the report is derived and does not
create a second source of truth. Manual bind/unbind operations edit those
fields through `openvlsilib`, and `Project::validate()` reports stale or duplicate
links. A layout cell may also carry one cell-level link to a schematic cell;
LVS prefers it over name inference, which lets separately authored schematic
and layout cells be paired explicitly. Geometry-extracted device anchors and
recursive hierarchical correspondence remain later extensions.

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

The extracted representation should be usable directly by the OpenVLSI simulation/benchmarking environment.

The current implementation is a first-order lumped extractor. It groups layout
geometry with the same connectivity rules as the layout connectivity view,
recognizes conducting layers by name (`Metal*`, `GatPoly`/`Poly*`,
`Activ`/diffusion, and via/contact layers), and creates or updates a text SPICE
schematic named `<layout> PEX` in the project hierarchy. Each net gets one
effective resistance from a small series/parallel conductor graph: touching
shapes merge at shared endpoints, parallel wires combine as conductances, and
via arrays at one location divide their resistance. The generated deck reports
area, perimeter, effective resistance, and total capacitance to ground for
each net. Single-terminal nets model the extracted resistance in series with
their capacitance to ground; multi-terminal nets use a resistance star between
their layout ports. Planned-device metadata from procedural layouts is extracted
first, using its model, parameter values, and terminal-net ids; this preserves
flattened PCell passives such as poly resistors and MIM capacitors in the
post-layout deck. Placed device-library instances are extracted when their
child cells provide terminal ports and model parameters; each terminal is
matched to the parent net that overlaps its transformed port geometry, and the
device is emitted as a SPICE primitive or PDK subcircuit. If no planned or
placed devices are recognized, the extractor falls back to copying the inferred
source schematic's device netlist before the parasitics.

Polygon devices are recognized through per-PDK rules in
`technology/extraction.json`. The rule engine intersects active/gate layers, cuts
active under gates so source and drain are not shorted, matches terminal layers
to the resulting regions, and emits MOS devices with geometric W/L. The same
rule file can declare two-terminal resistors and plate-overlap capacitors. This
is the Netgen/Magic-style extraction layer; PDK packages own their layer names,
models, and extraction requirements. Verify the result with LVS before treating
it as sign-off data.

Use the GUI action **Tools → Run PEX** (F9), `openvlsi-cli pex`, the MCP
`pex_run` tool, or the Python `openvlsipy.run_pex` helper. Coefficients are
configurable through the API/MCP surfaces and are recorded in the generated
deck so results remain traceable.

---

## 37. Manual + Automated Design

A central OpenVLSI philosophy should be that **automation and manual editing are not separate workflows**.

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

OpenVLSI should support experimental third-party plugins. During the pre-user
development phase, plugin APIs are allowed to change or break without
compatibility shims or migration paths. Plugin authors should target a specific
OpenVLSI build or source revision, and package metadata should record that target so
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

- No API or ABI compatibility guarantee is made between OpenVLSI builds.
- Plugins must declare the OpenVLSI build, source revision, or development API
  snapshot they target.
- `openvlsi-pkg` should reject or warn about plugins whose declared target does not
  match the installed OpenVLSI version.
- Plugin lifecycle hooks should still be explicit: discover, load, initialize,
  enable, disable, and unload where technically possible.
- Plugin dependencies and required capabilities should be declared in package
  metadata, even though the metadata schema may evolve.
- GUI/editor plugins may run in-process while the API is experimental.
- Plugins that execute untrusted code, invoke external tools, or can affect
  project integrity should prefer an out-of-process worker boundary.
- Plugin failures must be reported and must not silently corrupt the project
  database.
- Plugin operations must use `openvlsilib` services rather than maintaining a
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
OpenVLSI Application API
 │
 ▼
Domain Model
 │
 ├── Schematic
 ├── Layout ──▶ Native GDSII/OASIS interchange
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
 └── other tools
```

External dependencies should generally be accessed through adapters.

For example:

```text
OpenVLSI Layout Geometry
        │
        ▼
Geometry Kernel Interface
        │
        ▼
Clipper2 Backend
```

rather than spreading Clipper2 types throughout OpenVLSI.

Likewise:

```text
OpenVLSI Simulation API
        │
        ▼
ngspice Adapter
        │
        ▼
ngspice
```

and:

```text
OpenVLSI Analog Layout API
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
agentsdk-cpp (bundled submodule; shared MCP server + ACP client library; pulls in simdjson, libcurl, spdlog)
toml++ (TOML parsing)
spdlog (logging)
nanobind (C++ to Python bindings)
zlib (OASIS CBLOCK decompression)
```

### Domain backends

```text
ngspice
ALIGN
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

The user-facing ecosystem libraries are optional and should not become hard dependencies of the OpenVLSI executable.

### Developer tooling

```text
xmake (build system)
doctest (C++ unit tests)
Sphinx (documentation)
```

These run at build/docs time only and should not become hard dependencies of the OpenVLSI executable.

### Code quality and style

OpenVLSI must be solid as rock. Production code should favor correctness,
determinism, explicit error handling, strong type safety, and clear ownership
over shortcuts or fragile convenience abstractions. C++ and Qt code must follow
the [Qt Coding Style](https://wiki.qt.io/Qt_Coding_Style); reviews and automated
format/style checks should enforce the project conventions as they are added.

---

## 41. Unified Workflow

A complete OpenVLSI analog workflow should eventually look like:

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
              OpenVLSI Layout Editor
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

The agent should use OpenVLSI's structured APIs rather than interacting with Qt widgets as if it were a human.

---

## 43. Long-Term Vision

OpenVLSI should eventually function as a programmable IC design environment:

```text
                     ┌───────────────┐
                     │      OpenVLSI      │
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

> **OpenVLSI should have one canonical design model and one set of core operations,
> with `openvlsi`, `openvlsi-cli`, `openvlsi-pkg`, `openvlsi-mcp`, Python bindings, and the agent
> chatbox all acting as different interfaces to `openvlsilib`.**

That allows OpenVLSI to be simultaneously:

* A traditional schematic/layout editor
* A programmable layout environment
* An analog simulation environment
* An automated layout system
* A benchmarking/optimization framework
* A package ecosystem
* An AI-native EDA environment

without requiring separate implementations of the same functionality.
