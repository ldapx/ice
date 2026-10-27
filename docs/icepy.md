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
builder.polygon(
    "Metal2",
    [
        icepy.LayoutPoint(0, 1000),
        icepy.LayoutPoint(4000, 1000),
        icepy.LayoutPoint(2000, 3000),
    ],
)
builder.port("P1", "Metal1", 0, 0, 200, 200)
builder.parameter("w", "1u")

cell = builder.cell()
print(cell.name, len(cell.shapes), cell.bounds)
print(project.diagnostics())
project.save("demo.ice.json")
```

## PCell runtime

The upstream IHP PyCell implementations stay in the IHP checkout. `icepy`
provides the ICE-side runtime and transfers generated polygons directly into an
`icepy` layout cell:

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

ICE loads that package hook for layout-device regeneration. Building `icepy`
is therefore required for IHP PCell regeneration.

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
