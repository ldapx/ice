# icepy Python API

`icepy` is ICE's optional Python binding surface over the canonical `icelib`
project model. It is built with nanobind and is intended for automation,
notebooks, package implementations, and reproducible device generators.

## Build and import

```sh
xmake build icepy
export PYTHONPATH="$PWD/build/linux/x86_64/release:$PWD/python"
```

```python
import icepy

project = icepy.new_project("demo")
print(project.name, project.schema_version)
```

The compiled extension is `icepy.so`; the Python-side helpers are in
`python/icepy_pcells.py` and are exposed as `icepy.pcells` when that directory
is on `PYTHONPATH`.

## Project and layout model

The bindings expose the canonical project objects:

- `Project`, `LayoutCell`, `Layer`, `Shape`, `Port`, `LayoutInstance`
- `Parameter`, `Rect`, `LayoutPoint`, `DatabaseUnits`
- `Diagnostic` and `Severity`

`Project.layout_cells` and `Project.layers` are read-only snapshots. Use a
`LayoutBuilder` for mutation so IDs, layer lookup, and validation stay
consistent with the core:

```python
import icepy

project = icepy.new_project("demo")
builder = project.create_layout_cell("top", "ihp.sg13g2")

builder.layer("Metal1", 8)
builder.rectangle("Metal1", 0, 0, 4000, 600)
# Purpose-specific layers retain their GDS datatype.
builder.layer("Metal1.pin", 8, 2)
builder.polygon(
    "Metal2",
    [
        icepy.LayoutPoint(0, 1000),
        icepy.LayoutPoint(4000, 1000),
        icepy.LayoutPoint(2000, 3000),
    ],
)
builder.layout_net("VIN")
builder.port(
    "P1", "Metal1", 0, 0, 200, 200,
    direction="input", side="left", net="VIN", label="VIN",
)
builder.parameter("w", "1u")

cell = builder.cell()
print(cell.name, len(cell.shapes), cell.bounds)
print(project.diagnostics())
project.save("demo")
```

`Project.save()` and `Project.load()` use the split project-directory
format. The root contains `project.icepro`, `sch/*.icesch`,
`lay/*.icelay`, `sim/*.icesim`, `scripts/`, `build/`, `.gitignore`, and
`AGENTS.md`.

## Layout versus schematic

`icepy.run_lvs(project, schematic, layout)` compares a drawn schematic against a
layout and returns an `LvsReport` with `matched`, `schematic`, `layout`, and
`mismatches`. Each mismatch exposes its `category`, `message`, `severity`, and
optional schematic/layout object ids. `schematic_for_layout(project, layout)`
applies the same source-schematic inference used by the GUI and CLI, and
`lvs_report_json(report)` serializes the report.

```python
import icepy

report = icepy.run_lvs_named(project, "top", "top_layout")
if not report.matched:
    for mismatch in report.mismatches:
        print(mismatch.category, mismatch.message)
```

Use `run_lvs(project, schematic_id, layout_id)` when object ids are already
known, or `run_lvs_named(...)` with cell names.

LVS compares device presence, reference names, device types, shared parameters
on placed device instances, and net topology from planned connections. Text
SPICE schematics are not supported yet.

`icepy.run_pex(project, layout, options)` extracts first-order per-net
parasitics from a layout and creates or updates a text SPICE schematic in the
project. `run_pex_named(project, layout_name, options)` is the name-based
convenience form. The returned `PexReport` exposes `schematic_cell`, `spice_text`,
`total_resistance`, `total_capacitance`, per-net `PexNet` entries, extracted
`PexDevice` entries, `devices_from_layout`, `devices_from_schematic`, and notes.
`PexOptions` controls sheet resistances, capacitance coefficients, whether
placed layout devices are extracted, whether polygon devices are extracted from
the PDK rules, an optional extraction-rule path override, whether the linked
schematic netlist is used as a fallback, and the generated cell name.
`pex_report_json(report)` serializes the report.

```python
options = icepy.PexOptions()
options.metal_sheet_resistance = 0.08
report = icepy.run_pex(project, layout_id, options)
print(report.schematic, report.total_resistance, report.total_capacitance)
```

## Simulation metrics

Simulation result loading and metrics live in the pure-Python `icepy_sim`
module, also exposed as `icepy.sim` when the extension is built:

```python
from icepy import sim

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

The API is new-format-only and reads runs from `build/simulations`.

## PCell runtime

`icepy.pcells` has two package-facing paths.

**ICE-native package PCells** use `PackagePcell` subclasses stored in
`<package>/layout/pcells.py`. Each class declares its parameter metadata,
logical layer map, and device family. The runtime builds it through the normal
`icepy` layout builder and returns JSON geometry:

```python
from icepy import pcells

geometry = pcells.generate_package_geometry(
    "/path/to/ice-pkg-sky130", "pcells", "nfet", {"w": "2u", "l": "0.2u"}
)
```

`generate_package_geometry(...)` is the contract behind
`layout/device_generator.py` and `layout/generate_devices.py`. The Sky130 and
GF180MCU packages use this path, so their PCell replicas are defined entirely
with the ICE layout API.

**IHP PyCell runtime.** The upstream IHP PyCell implementations are vendored
inside the IHP package under `upstream/ihp`. `icepy` provides the ICE-side
runtime, transfers generated polygons directly into an `icepy` layout cell, and
converts the PyCell `MkPin` terminal declarations into layout pins:

```python
import icepy

project = icepy.new_project("nmos_demo")
cell = icepy.pcells.generate_cell(
    project,
    "IHP nmos_1",
    "/path/to/IHP",
    "nmos_code",
    "nmos",
    {"w": "1u", "l": "0.13u", "ng": "1"},
)
print(len(cell.shapes), cell.bounds)
```

`icepy.pcells.generate_geometry(...)` returns the legacy device-library JSON
shape (`bounds` plus layer/datatype/polygon records), while
`parameter_specs_for(...)` returns the PCell parameter definitions.

The IHP package uses this runtime through:

```text
ice-pkg-ihp/layout/device_generator.py
```

ICE loads that package hook for layout-device regeneration, and the package
passes its vendored `upstream/ihp` tree as the IHP root. Building `icepy` is
therefore required for IHP PCell regeneration.

The generic package generator contract is:

```text
<pdk-root>/layout/device_generator.py --pdk-root <package> \
    --module <module> --class <class> \
    --params-file <params.json> --output <geometry.json>
```

`icepy.pcells.package_main_cli(...)` implements this contract for ICE-native
package PCells.

## Package implementation guidance

Package Python code should use generic `icepy` APIs rather than mutating
serialized project JSON directly. A package-provided generator should:

1. receive explicit parameters,
2. create or add to a layout cell through `Project.create_layout_cell`,
3. add geometry through `LayoutBuilder`,
4. return only data that ICE can validate.

This keeps PDK-specific logic in the package while preserving the canonical
core model and transaction guarantees.

## Smoke test

```sh
PYTHONPATH=build/linux/x86_64/release:python python3 tests/icepy_smoke.py
```

The test exercises the layout builder and project round-trip. If
`ICE_IHP_ROOT` is set, or an `IHP` checkout exists next to the ICE repository,
it also generates the IHP nmos PCell through `icepy.pcells`.

Package PCell regression coverage is available separately:

```sh
PYTHONPATH=build/linux/x86_64/release:python python3 tests/package_pcells_test.py
```

It validates the package libraries and regenerates every declared PCell through
the package generator contract.
