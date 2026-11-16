# ICE Packages

Packages are directories containing a versioned `package.toml` manifest and
capability-oriented content such as `symbols/`, `technology/`, `layout/`,
`simulation/`, `implementation/`, and `vendor/`.

`ice-pkg resolve <repository> <ice-version> name=constraint ...` selects the
highest matching local package version, resolves transitive dependencies, and
writes a deterministic `ice.lock`. The lockfile records exact versions,
sources, trust declarations, capabilities, canonical `pdk_id`, and SHA-256
content hashes. The content hash covers the package model files, so a PDK
package version plus hash pins the simulation stack.

Package code is untrusted by default. A package's `[trust] trusted = true`
metadata is recorded for policy decisions but does not grant execution
permission by itself. Scripts and plugins must remain behind an explicit
permission boundary.

The example packages under `examples/packages` demonstrate a minimal PDK,
library dependency, symbol metadata, and SVG artwork. The default index ships
separate packages for:

- `ihp.sg13g2` — IHP SG13G2 130 nm BiCMOS
- `sky130` — SkyWater SKY130 130 nm CMOS
- `gf180mcu` — GlobalFoundries GF180MCU 180 nm CMOS

External Git packages can be installed into a local repository with:

```text
ice-pkg install <repository> <name> <version> <git-url> [revision]
```

The installer clones the source and writes an ICE wrapper manifest when the
repository does not already provide one. It does not execute package scripts.
The IHP, Sky130, and GF180 packages now vendor their simulation model subsets
and declare a `[pdk.simulation]` contract; an upstream Volare/CIEL installation
is not needed for simulation.

## PDK package directory layout

The bundled PDK packages use one canonical tree. Optional directories are
omitted when a PDK does not need them.

```text
<pdk-package>/
├── package.toml
├── README.md
├── LICENSE
├── ATTRIBUTION.md
├── .github/logo.png
├── symbols/                       # schematic sidecars
├── technology/
│   ├── layer-styles.json
│   └── extraction.json
├── layout/
│   ├── devices/
│   │   ├── index.json
│   │   ├── pcell/                 # Python PCell definitions
│   │   ├── fixed/                 # fixed .icelay cells
│   │   ├── generator.py
│   │   └── build.py
│   └── dummy_fill.py              # optional procedural-fill hook
├── simulation/
│   ├── ngspice/
│   ├── verilog-a/
│   └── osdi/                      # optional built outputs
├── implementation/
│   ├── common/
│   │   ├── cells.lef
│   │   ├── stdcells.gds
│   │   ├── layer.map
│   │   ├── lib/
│   │   └── scripts/
│   └── stacks/
│       └── <stack>/
│           ├── tech.lef
│           └── rcx.rules
└── vendor/                        # optional upstream snapshots
```

Shared implementation data lives under `implementation/common/`. Per-stack
technology LEF and RCX rules live under `implementation/stacks/<stack>/`;
single-stack PDKs use `implementation/stacks/default/`. Liberty files may be
plain `.lib` or gzip-compressed `.lib.gz`. Built OSDI objects are cached outside
the package by `ice-pkg pdk-build`; their Verilog-A sources live under
`simulation/verilog-a/`. All manifest paths are relative to the package root.

## PDK metadata

Simulation PDKs come from ICE packages only. Volare, CIEL, `PDK_ROOT`, `PDK`,
and system PDK roots are not runtime fallbacks.

A package that provides a PDK should declare a canonical ID, aliases, and a
simulation contract in `package.toml`:

```toml
[pdk]
id = "ihp-sg13g2"
aliases = ["ihp.sg13g2"]
display_name = "IHP SG13G2"
manufacturing_grid = 5

[pdk.simulation]
engine = "ngspice"
root = "simulation/ngspice"
model_file = "models/cornerMOSlv.lib"
section = "mos_tt"
requires_osdi = true
verilog_a = "simulation/verilog-a"

[pdk.simulation.devices]
nmos = "sg13_lv_nmos"
pmos = "sg13_lv_pmos"
```

`manufacturing_grid` is the PDK coordinate grid in ICE database units
(nanometres). ICE uses it as the default layout grid for new layouts while
preserving a project grid the user has changed. It does not replace per-layer
minimum-spacing rules or placement/routing track pitches.

Package simulation paths are relative to the package root. OSDI packages
declare each Verilog-A source and output:

```toml
[[pdk.simulation.osdi]]
source = "simulation/verilog-a/psp103/psp103.va"
output = "psp103.osdi"
```

Build the OSDI cache once with:

```sh
ice-pkg pdk-build ihp.sg13g2
```

A package may also declare its display name, default schematic, layout layers,
and one or more process stack variants:

```toml
[pdk]
display_name = "SkyWater SKY130"
default_schematic = "sky130_top"
default_stack = "5LM_1TM"
manufacturing_grid = 5

[[pdk.layer]]
name = "nwell.drawing"
number = 64
datatype = 20
minimum_width = 840

[[pdk.layer]]
name = "diff.drawing"
number = 65
datatype = 20
minimum_width = 150

[[pdk.stack]]
id = "3LM_1TM"
display_name = "3 metal layers + 1 thick top"
layers = ["nwell.drawing", "diff.drawing", "metal1.drawing"]

[[pdk.stack]]
id = "6LM_1TM"
display_name = "5 metal layers + MetalTop"
layers = ["nwell.drawing", "diff.drawing", "metal1.drawing",
          "metal2.drawing", "metal3.drawing", "metal4.drawing",
          "metal5.drawing", "metaltop.drawing"]
```

Each stack lists the PDK layers it exposes. Entries may name either a full
layer (`metal1.drawing`) or its base (`metal1`); purpose layers such as
`metal1.pin` follow their base entry. `minimum_width` is the layer's minimum
drawn width in nanometres; the Path tool uses it as the default and refuses to
draw narrower paths on that layer. A layout cell stores exactly one PDK and
one stack; layers added for that layout carry the same stack and only appear in
layouts using it. `default_stack` is preselected when a layout is
created and is used when a layout is generated from a schematic. A PDK
without `[[pdk.stack]]` entries has a single implicit stack and works as
before. Projects may use layouts from several PDKs, but a layout instance
must reference a child with the same PDK and stack as its parent.

Activating the package adds these layers to the Layout editor, records the
package as a project dependency, and creates the default schematic when the
project has no schematics yet. Layout layers store both the GDS number and
datatype. Creating a layout adds the package's full device-library layer set
for the selected stack, so placing devices only reuses existing layers.
Device libraries use purpose-qualified names such as `GatPoly.pin` and
`Metal1.nofill`; purpose layers with non-zero datatypes and marker layers
start hidden, while drawing purposes remain visible. The Layers panel groups
purposes under their base layer number.

### Implementation platform data

A PDK package can also describe the physical data required by the RTL-to-GDS
runner. Declare `implementation = true` under `[provides]`, then declare the
engine and optional ORFS compatibility mapping once per PDK:

```toml
[pdk.implementation]
engine = "openroad"
synthesis = "yosys"
min_openroad = "2.0"
min_yosys = "0.40"

[pdk.implementation.orfs]
platform = "gf180"
track_option = "9t"
power_option = "5v0"
corner = "BC"
liberty_pattern = "mcu9t5v0__tt_025C_5v00"
```

Explicit file lists belong to the PDK stack. These paths are relative to the
package root and are covered by the package content hash:

```toml
[pdk.stack.implementation]
site = "GF018hv5v_green_sc9"
tech_lef = "implementation/stacks/5LM_1TM/tech.lef"
cell_lef = "implementation/common/cells.lef"
gds = "implementation/common/stdcells.gds"
layer_map = "implementation/common/layer.map"
io_horizontal_layers = ["Metal3"]
io_vertical_layers = ["Metal4"]
routing_min_layer = "Metal1"
routing_max_layer = "Metal5"
tapcell_script = "implementation/common/scripts/tapcell.tcl"
pdn_script = "implementation/common/scripts/pdn.tcl"
rc_script = "implementation/common/scripts/setRC.tcl"
rcx_rules = "implementation/stacks/5LM_1TM/rcx.rules"
fill_cells = ["FILL1", "FILL2"]

[pdk.stack.implementation.liberty]
tt = ["implementation/common/lib/tt.lib.gz"]
ff = ["implementation/common/lib/ff.lib.gz"]
ss = ["implementation/common/lib/ss.lib.gz"]
```

`layer_map` is a KLayout LEF/DEF layer-map file. The final GDS export uses it to
stream routed DEF geometry onto the PDK's real GDS layer numbers. Without a
map, KLayout assigns sequential numbers to LEF layers, so routes, pins, labels,
and the die outline land on layers that do not exist in the PDK.

When `[pdk.implementation.orfs]` is present, ICE may resolve missing LEF,
Liberty, site, IO-layer, routing, OpenRCX rule, and standard-cell GDS defaults
from an OpenROAD-flow-scripts platform under `OPENROAD_FLOW_SCRIPTS` or
`ICE_OPENROAD_FLOW_SCRIPTS`. The Liberty pattern should include the standard-cell
variant (for GF180MCU, `mcu9t5v0` for 9-track data) so Liberty masters match the
cell LEF. Explicit per-stack paths take precedence. `implementation-toolchain`
reports whether `openroad`, `yosys`, and `klayout` are available.

### Layer styles

A PDK package can ship default colors, patterns, and visibility for its layers
in `technology/layer-styles.json`. ICE discovers the file automatically from the
package root; it is not listed in `package.toml`.

```json
{
  "schema_version": 1,
  "layers": [
    {
      "name": "Metal1",
      "color": "#3498db",
      "pattern": "Solid",
      "visible": true
    },
    {
      "name": "Metal1.pin",
      "color": "#3498db",
      "pattern": "Dense dots",
      "visible": false
    }
  ]
}
```

`name` matches either a full layer name or its base name before the first
`.`; a base-name entry applies to every purpose of that layer unless the full
name has its own entry. `pattern` accepts the names shown in the layer pattern
menu (for example `Solid`, `Diagonal /`, `Cross hatch`, `Sparse dots`).
Styles are applied when the package's layers are added to a project. Layers
without a style entry keep the built-in number-based palette. The IHP SG13G2,
Sky130, and GF180MCU packages ship style files derived from their layer maps.

## Package indexes

The GUI reads available packages from a Git package index. The default index is
https://github.com/ldapx/ice-packages, cloned into `~/.ice/package-index`.
Opening **Manage Packages** automatically clones the default index when it is
missing or empty, so the repository's packages are shown without pressing
**Refresh**. **Refresh** still performs an explicit pull and submodule update.

Indexed packages are Git submodules. Each package directory contains a
`package.toml` manifest and may provide `.github/logo.png`, which the package
manager displays as the package logo. The submodule URL and branch recorded in
`.gitmodules` are used when installing the package.

## GUI package manager

Open `Project -> Manage Packages` to:

- browse every package in the index with its description, version, capabilities,
  and `.github/logo.png` logo;
- install an indexed package into `~/.ice/packages`;
- install a package from an arbitrary repository URL with **Install from
  Repository URL…**;
- uninstall any installed package, including packages that are not currently in
  the index;
- refresh the index and initialize its submodules;
- activate any installed PDK package when a project is open.

Installing and uninstalling packages work without an open project. Installing a
package writes an `ice.lock` beside the project when a project is open. A
package that was removed from the index but is still installed remains visible
so it can be uninstalled.

## Project package dependencies

Every project records its package dependencies. Opening a project verifies that
each dependency is installed. If packages are missing, ICE lists them and offers
to install them before the project opens; closing the prompt leaves the project
closed.

Creating a schematic asks for its PDK. The built-in **Generic PDK** is always
available. Installed packages that provide the `pdk` capability appear as
additional choices. Selecting a package PDK adds that package to the project
dependency list after confirmation; declining the prompt cancels schematic
creation. The schematic stores its PDK in the project file.

A schematic using the Generic PDK browses only the built-in generic symbols. A
schematic using a package PDK browses the built-in generic symbols plus that
package's `symbols/` library. Activating a PDK package adds its declared
technology layers to the Layout editor and records the package as a project
dependency.

### Symbol pins and SPICE nodes

Bipolar and MOS device models are SPICE subcircuits, so a symbol must declare
one pin per subcircuit node, in the order the model expects. ICE reads the node
list from the package model files, therefore a bipolar symbol with a substrate
pin (for example the SKY130 NPN, declared as `.subckt ... c b e s`) is
instantiated with four nodes, while a three terminal substrate PNP keeps three.
When a symbol omits the substrate pin and the model declares one, the netlist
grounds that node and reports a note so a stale symbol can be corrected. A
symbol that is placed more than once is copied per instance (see
`schematic_add_instance`), which keeps pins and net connectivity unambiguous.

### Device geometry units

The package simulation options are applied before the schematic netlist, so a
`.option scale=...` line (SKY130 declares `.option scale=1u`) sets the length
unit of every MOS width and length. ICE converts sizes written with an SI
suffix (`1u`, `150n`) into those units and emits bare values unchanged, so a
symbol default of `w = 1u` becomes `W=1` and a hand written `W=60` stays 60
microns. Without a scale option, sizes are emitted verbatim.

## PDK-provided procedural layout operations

Procedural layout scripts can request process-specific geometry through the
generic layout builder API. ICE dispatches these requests to the active PDK
package after the user script finishes:

```python
layout.add_dummy_fill(layers=["Metal1", "Metal2"], density=50, deviation=10)
```

For dummy fill, the package contract is `layout/dummy_fill.py` with an
`add_dummy_fill(layout, **options)` function. The implementation may inspect
the builder through `layout.iter_polygons()`, `layout.bounds()`, and
`layout.layer_number()`, and it adds geometry through the normal
`layout.layer(...).rectangle()` / `.polygon()` API. Calling
`layout.add_dummy_fill(...)` is an explicit request to execute package code;
PDKs without the implementation report a clear error. The IHP SG13G2 package
implements dummy metal fill for its metal stack using rules adapted from
`gdsfill`.

The File menu also provides **Open GDS/OASIS Layout** and **Save GDS/OASIS
Layout**. These actions use native GDSII (`*.gds`, `*.gdsii`) for the documented
deterministic BOUNDARY-only subset. OASIS extensions are accepted for workflow
compatibility but OASIS import/export is explicitly rejected until a native
OASIS adapter exists.

## PDK-provided layout libraries and device generators

Each PDK package owns its layout device library:

```text
<pdk-root>/layout/devices/index.json       # library index
<pdk-root>/layout/devices/fixed/*.icelay   # fixed-device cells (optional)
<pdk-root>/layout/devices/pcell/           # Python PCell definitions
<pdk-root>/layout/devices/generator.py     # runtime PCell generator
<pdk-root>/layout/devices/build.py         # index/fixed-device regeneration
<pdk-root>/technology/extraction.json      # polygon device extraction rules
<pdk-root>/technology/drc.json             # external DRC deck selection (optional)
```

Parameterized devices are Python PCells: the index carries their generator
metadata (`module` and `class`) and parameter defaults, but no stored geometry.
ICE generates their layout from the package Python scripts when a device is
selected or placed, and regenerates it when a parameter changes. Fixed devices
have no generator metadata and are stored as `layout/devices/fixed/*.icelay`
cells.
The `index.json` index also carries the library layers, ordering, categories,
and a `cell_prefix` used for instance names (for example `IHP `, `SKY130 `, or
`GF180 `). No global PDK installation is consulted.

The IHP SG13G2 package ships its complete device library plus vendored IHP
PyCell sources under `vendor/ihp`. Parametric regeneration evaluates those
sources through `icepy.pcells` and transfers polygons into an `icepy` layout
cell.

The Sky130 and GF180MCU packages ship ICE-native replicas of their PCell
families in `layout/devices/pcell/pcells.py`. They use the `icepy` layout
builder API directly and do not require KLayout, gdsfactory, Volare, CIEL, or
`PDK_ROOT`. Their `layout/devices/index.json` is generated from the same
definitions with `layout/devices/build.py`. Parameterized cells are written as
PCell index entries without geometry; only fixed cells are written as
`layout/devices/fixed/*.icelay`.

`technology/drc.json` declares how ICE runs DRC for the package. It can point
at a KLayout `.lydrc`/`.drc` deck (with `{deck}`, `{input}`, `{report}`,
`{top_cell}`, `{suite}`, and `{workdir}` placeholders) or a gdscheck process,
and may list search paths for an externally installed PDK. The SKY130 package
uses this to run the SkyWater KLayout signoff deck from `$PDK_ROOT` or Volare.
Named `suites` can select alternate decks or rule groups. `ICE_DRC_DECK`
overrides the deck path for every PDK, and `ICE_DRC_DECK_<PDK>` (for example
`ICE_DRC_DECK_SKY130A`) overrides it for one PDK.

`ice-cli pdk-info <pdk>` reports the resolved `layout_library`,
`layout_generator`, and `layout_extraction` paths for each installed package.

### Polygon extraction rules

`technology/extraction.json` declares how PEX recognizes devices drawn directly as
polygons. The rule engine is intentionally declarative so every PDK package
owns its layer names, models, and terminal layers:

```json
{
  "format": "ice-device-extraction",
  "schema_version": 1,
  "pdk": "ihp-sg13g2",
  "devices": [
    {
      "name": "nmos",
      "type": "mos",
      "model": "sg13_lv_nmos",
      "element": "X",
      "active": "Activ",
      "gate": "GatPoly",
      "exclude_layers": ["NWell", "ThickGateOx"],
      "terminal_layers": ["Contact", "Metal1"],
      "substrate": "0",
      "length_axis": "auto"
    }
  ]
}
```

Supported rule types are:

- `mos`: intersect `active` and `gate`, split the active region at the gate,
  and find source/drain nets from `terminal_layers`. `require_layers` and
  `exclude_layers` distinguish NMOS/PMOS and voltage variants. `body_layer`
  supplies the bulk net when present.
- `resistor`: use a `body` layer, `sheet_resistance`, and terminal layers.
- `capacitor`: intersect two `plates` and apply `capacitance_per_area` in
  fF/um^2.

PEX first cuts active layers under MOS gates so source and drain do not appear
as one shorted diffusion net. It then emits a SPICE device per channel. Device
dimensions are derived from the channel geometry: the gate aspect ratio chooses
the length axis unless `length_axis` is explicitly `x` or `y`.

Building `icepy` is required for PDK device regeneration.
