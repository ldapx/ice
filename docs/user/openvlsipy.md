# openvlsipy Python API

`openvlsipy` is OpenVLSI's optional Python binding surface over the canonical `openvlsilib`
project model. It is built with nanobind and is intended for automation,
notebooks, package implementations, and reproducible device generators.

## Build and import

```sh
xmake build openvlsipy
export PYTHONPATH="$PWD/build/linux/x86_64/release:$PWD/python"
```

```python
import openvlsipy

project = openvlsipy.new_project("demo")
print(project.name, project.schema_version)
```

The compiled extension is `openvlsipy.so`; the Python-side helpers are in
`python/openvlsipy_pcells.py` and are exposed as `openvlsipy.pcells` when that directory
is on `PYTHONPATH`.

## Project and layout model

The bindings expose the canonical project objects:

- `Project`, `LayoutCell`, `Layer`, `Shape`, `Port`, `LayoutInstance`
- `LayoutDeviceBinding`, `LayoutDeviceKind`
- `Parameter`, `Rect`, `LayoutPoint`, `DatabaseUnits`
- `Diagnostic` and `Severity`
- `LvsReport`, `LvsDeviceMatch`, `LvsNetMatch`, `LvsPortMatch`, `LvsMismatch`,
  `LvsObjectRef`, `LvsSummary`, `LvsCandidate`

`Project.layout_cells` and `Project.layers` are read-only snapshots. Use a
`LayoutBuilder` for mutation so IDs, layer lookup, and validation stay
consistent with the core:

```python
import openvlsipy

project = openvlsipy.new_project("demo")
builder = project.create_layout_cell("top", "ihp.sg13g2")

builder.layer("Metal1", 8)
rect = builder.rectangle("Metal1", 0, 0, 4000, 600)
# Purpose-specific layers retain their GDS datatype.
builder.layer("Metal1.pin", 8, 2)
builder.polygon(
    "Metal2",
    [
        openvlsipy.LayoutPoint(0, 1000),
        openvlsipy.LayoutPoint(4000, 1000),
        openvlsipy.LayoutPoint(2000, 3000),
    ],
)
builder.layout_net("VIN")
# The layer argument is required and is stored as the port's routing layer.
builder.port(
    "P1", "Metal1", 0, 0, 200, 200,
    direction="input", side="left", net="VIN", label="VIN",
)
builder.parameter("w", "1u")

# Copies preserve path segment layers, widths, and vias.
builder.duplicate([rect], dx=5000)
builder.array([rect], rows=2, columns=3, x_pitch=5000, y_pitch=1000)
rect2 = builder.rectangle("Metal1", 8000, 0, 100, 100)
builder.arrange([rect, rect2], "left")

cell = builder.cell()
print(cell.name, len(cell.shapes), cell.bounds)
print(project.diagnostics())
project.save("demo")
```

`LayoutBuilder.duplicate`, `array`, and `arrange` use the same core operations
as the desktop editor and CLI. `duplicate` returns new shape ids and defaults
its offset to one selection width plus one grid step. `array` returns the new
copies. `arrange` accepts `left`, `right`, `top`, `bottom`,
`center-horizontal`, `center-vertical`, `distribute-horizontal`, or
`distribute-vertical`.

`Project.create_layout_from_schematic(schematic, name="", stack="")` projects a
drawn schematic into a layout and returns a `LayoutBuilder`. The builder exposes
the cell-level link through `link_schematic(schematic_cell)` and
`unlink_schematic()`, and the layout cell exposes `schematic_cell`.
The builder also exposes the authored device links through
`device_links(schematic_cell=0)`, and
`bind_device(layout_device, schematic_cell, schematic_instance)` /
`unbind_device(layout_device)` edit them. Bindings are normal project
mutations, so they participate in undo and serialization. `LayoutDeviceBinding`
reports the layout object, its kind (`planned_device` or `instance`), the
resolved schematic cell and instance names, and `stale`/`duplicate` status.

`Project.save()` and `Project.load()` use the split project-directory
format. The root contains `project.ovpro`, `sch/*.ovsch`,
`lay/*.ovlay`, `sim/*.ovsim`, `scripts/`, `build/`, `.gitignore`, and
`AGENTS.md`.

## Layout versus schematic

`openvlsipy.run_lvs(project, schematic, layout)` compares a drawn schematic against a
layout and returns an `LvsReport` with `matched`, `schematic`, `layout`,
`summary`, `devices`, `nets`, `ports`, and `mismatches`. Each mismatch exposes
its `category`, `message`, `severity`, optional schematic/layout object ids,
typed `schematic_ref`/`layout_ref`, a `hint`, and candidate objects.
`schematic_for_layout(project, layout)` applies the same source-schematic
inference used by the GUI and CLI, and `lvs_report_json(report)` serializes the
report.

```python
import openvlsipy

report = openvlsipy.run_lvs_named(project, "top", "top_layout")
if not report.matched:
    for mismatch in report.mismatches:
        print(mismatch.category, mismatch.message, mismatch.hint)
for match in report.devices:
    print(match.schematic.name, match.layout.name, match.method, match.matched)
```

Use `run_lvs(project, schematic_id, layout_id)` when object ids are already
known, or `run_lvs_named(...)` with cell names.

LVS compares device presence, reference names, device types, shared parameters
on placed device instances, terminal-aware net topology, and text SPICE or
structural SystemVerilog source cells.

`openvlsipy.run_pex(project, layout, options)` extracts first-order per-net
parasitics from a layout and creates or updates a text SPICE schematic in the
project. `run_pex_named(project, layout_name, options)` is the name-based
convenience form. The returned `PexReport` exposes `schematic_cell`, `spice_text`,
`total_resistance`, `total_capacitance`, per-net `PexNet` entries, extracted
`PexDevice` entries, `devices_from_layout`, `devices_from_schematic`, and notes.
`PexOptions` controls sheet resistances, capacitance coefficients, whether
planned or placed layout devices are extracted, whether polygon devices are extracted from
the PDK rules, an optional extraction-rule path override, whether the linked
schematic netlist is used as a fallback, and the generated cell name.
`pex_report_json(report)` serializes the report.

```python
options = openvlsipy.PexOptions()
options.metal_sheet_resistance = 0.08
report = openvlsipy.run_pex(project, layout_id, options)
print(report.schematic, report.total_resistance, report.total_capacitance)
```

## Simulation metrics

Simulation result loading and metrics live in the pure-Python `openvlsipy_sim`
module, also exposed as `openvlsipy.sim` when the extension is built:

```python
from openvlsipy import sim

run = sim.load_latest("/path/to/project", simulation="rlc_sim")
if run is not None:
    metrics = run.trace("v(n1)").metrics(settling_tolerance=0.02)
    print(metrics.overshoot_percent)
    print(metrics.settling_time)
    print(metrics.ringing_frequency)
```

Compare multiple simulations:

```python
runs = sim.load_latest_many(
    "/path/to/project",
    simulations=["rlc_damped", "rlc_tuned"],
)
for name, run in runs.items():
    print(name, run.trace("v(n1)").metrics().settling_time)
```

Each loaded run also exposes `analysis`, `x_axis`, `x_unit`, `variant`,
`scalars`, and the declarative `measurements` stored with the run. `variant`
identifies corner/Monte Carlo experiment points. The API is new-format-only and
reads runs from `build/simulations`.

## Procedural layouts

`openvlsipy.Project` exposes procedural layouts alongside static layout cells. See
{doc}`procedural-layout` for the script-side `layout` builder API, units,
devices, routing, and the generated API reference.

```python
project = openvlsipy.new_project("generated_demo")
procedural = project.create_procedural_layout(
    "generated_top",
    "generic",
    "",
    'width = layout.um(layout.parameter("width", "4"))\n'
    'metal = layout.layer("Metal1", 1, 0)\n'
    'metal.rectangle(layout.um(0), layout.um(0), width, layout.um(1))\n',
    {"width": "4"},
)
assert procedural.needs_regeneration
procedural = project.regenerate_procedural_layout("generated_top")
assert not procedural.needs_regeneration
cell = project.layout_cell_by_id(procedural.generated_cell)
```

The script API includes datatype-aware layers, unit helpers, rectangles,
polygons (including holes), boolean operations, paths, text, vias, rectangular
arrays, ports, instances, package device placement through
`layout.device(...)` with an installed PDK package, and Manhattan routing with
`layout.route(...)`/`layout.connect(...)`. A placement returns a
`DevicePlacement` with absolute port rectangles and `device`/`model` metadata;
pass `nets={"G": "GATE", ...}` to bind device ports. The generated cell also stores
planned-device metadata, so LVS can match flattened PCells by model, W/L/M, and
terminal nets. Generated children are read-only and can be inspected, exported,
or instanced by other layout cells. `ProceduralLayout` exposes the generated
script/output hashes and `needs_regeneration`.

## PCell runtime

`openvlsipy.pcells` is the technology-neutral package PCell runtime. It has two
package-facing paths. The shared `PackagePcell` MOS
template builds isolated fingers (one diff island per finger), licon/mcon
contact arrays, body taps, and well/implant layers. Packages can override
`contact_size`, `contact_spacing`, and `contact_margin`; SKY130 uses 0.17/0.19
µm and GF180MCU uses 0.22/0.25 µm. Device width is the channel width per finger,
and `nf` produces parallel fingers instead of overlapping diffusion.

**OpenVLSI-native package PCells** use `PackagePcell` subclasses stored in
`<package>/layout/devices/pcell/pcells.py`. Each class declares its parameter metadata,
logical layer map, and device family. The runtime builds it through the normal
`openvlsipy` layout builder and returns JSON geometry:

```python
from openvlsipy import pcells

geometry = pcells.generate_package_geometry(
    "/path/to/openvlsi-pkg-sky130", "pcells", "nfet", {"w": "2u", "l": "0.2u"}
)
```

`generate_package_geometry(...)` is the contract behind
`layout/devices/generator.py` and `layout/devices/build.py`. The Sky130 and
GF180MCU packages use this path, so their PCell replicas are defined entirely
with the OpenVLSI layout API.

**IHP PyCell runtime.** The upstream IHP PyCell implementations are vendored
inside the IHP package under `vendor/ihp`. The package owns the OpenVLSI-side bridge
in `layout/devices/pcell/ihp_runtime.py`; it bootstraps the CNI/PyCell layer,
transfers generated polygons directly into an `openvlsipy` layout cell, and converts
the PyCell `MkPin` terminal declarations into layout ports:

```python
import sys
from pathlib import Path
import openvlsipy

sys.path.insert(0, "/path/to/openvlsi-pkg-ihp-sg13g2/layout/devices/pcell")
from ihp_runtime import generate_cell

project = openvlsipy.new_project("nmos_demo")
cell = generate_cell(
    project,
    "IHP nmos_1",
    "/path/to/openvlsi-pkg-ihp-sg13g2/vendor/ihp",
    "nmos_code",
    "nmos",
    {"w": "1u", "l": "0.13u", "ng": "1"},
)
print(len(cell.shapes), cell.bounds)
```

`ihp_runtime.generate_geometry(...)` returns the device-library JSON shape
(`bounds` plus layer/datatype/polygon records), while
`parameter_specs_for(...)` returns the PCell parameter definitions.

The package exposes this runtime through:

```text
openvlsi-pkg-ihp/layout/devices/generator.py
```

OpenVLSI loads that package hook for layout-device regeneration, and the generator
passes its vendored `vendor/ihp` tree as the IHP root. Building `openvlsipy` is
therefore required for IHP PCell regeneration, but no IHP-specific runtime code
is required in the OpenVLSI repository.

The generic package generator contract is:

```text
<pdk-root>/layout/devices/generator.py --pdk-root <package> \
    --module <module> --class <class> \
    --params-file <params.json> --output <geometry.json>
```

`openvlsipy.pcells.package_main_cli(...)` implements this contract for OpenVLSI-native
package PCells.

## Schematic, simulation, DRC, and packages

`Project.create_schematic_cell`, `create_text_schematic_cell`, and
`create_systemverilog_cell` return a `SchematicBuilder` with `symbol`, `pin`,
`instance`, `net`, `component`, `set_parameter`, `set_cell_parameter`,
`netlist`, and `wrap`. `Project.schematic_cells`, `schematic_cell`, and
`schematic_cell_by_id` expose parsed cell data. `SchematicBuilder.wrap`
moves selected instance ids into a child schematic, returns a
`SchematicWrapResult` with the child cell, generated symbol, parent instance,
and port ids, and preserves internal and boundary connectivity.

```python
builder = project.create_schematic_cell("top")
resistor = builder.symbol("resistor")
a = builder.pin(resistor, "A", "left", -50, 0)
b = builder.pin(resistor, "B", "right", 50, 0)
r1 = builder.instance(resistor, "R1")
builder.set_parameter(r1, "resistance", "10k")
wrapped = builder.wrap([r1], "wrapped")
print(wrapped.cell, wrapped.instance)
```

Simulation definitions use `SimulationItem`. Set the schematic id, analysis,
simulator, stop/step, corners, sweeps, Monte Carlo options, measurements, and
graphs, then call `Project.create_simulation`, `update_simulation`, or
`run_simulation`.

```python
simulation = openvlsipy.SimulationItem()
simulation.name = "tran"
simulation.schematic = builder.id
simulation.stop = 1e-6
simulation.step = 1e-9
simulation.capture_signals = "v(out)"
project.create_simulation(simulation)
result = project.run_simulation("tran", project_root=".")
```

Verification exposes `LocalVerificationAdapter`, `run_layout_drc`,
`parse_lyrdb`, `parse_lyrdb_text`, and `verification_report_json`. Package
management exposes `PackageManager.read_manifest`, `content_hash`,
`install_git`, `install_git_package`, `uninstall`, `scan_index`, `resolve`,
`read_lockfile`, `write_lockfile`, `verify_lockfile`, `capabilities`, and
`provenance`, plus `package_matches_pdk` and `resolve_pdk_stack`.

## Package implementation guidance

Package Python code should use generic `openvlsipy` APIs rather than mutating
serialized project JSON directly. A package-provided generator should:

1. receive explicit parameters,
2. create or add to a layout cell through `Project.create_layout_cell`,
3. add geometry through `LayoutBuilder`,
4. return only data that OpenVLSI can validate.

This keeps PDK-specific logic in the package while preserving the canonical
core model and transaction guarantees.

## Smoke test

```sh
PYTHONPATH=build/linux/x86_64/release:python python3 tests/openvlsipy_smoke.py
```

The test exercises the layout builder and project round-trip. If the IHP
package is installed, it also generates the IHP nmos PCell through the
package's `ihp_runtime.py` bridge.

Procedural autorouter coverage is available separately:

```sh
python3 tests/procedural_router_test.py
```

Package PCell regression coverage is available separately:

```sh
PYTHONPATH=build/linux/x86_64/release:python python3 tests/package_pcells_test.py
```

It validates the package libraries and regenerates every declared PCell through
the package generator contract.
