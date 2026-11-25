# Procedural Layout API

Procedural layouts are Python scripts that generate a static layout cell from
parameters. A procedural layout is stored as a script plus a parameter set; its
generated cell is a read-only child that is replaced whenever the script or a
parameter changes. The same generated cell feeds the GUI canvas, GDS export,
LVS, PEX, and hierarchy instances.

The script runs in a separate Python process with a single injected object named
`layout`. Scripts build geometry through that builder; they do not mutate the
project database directly. ICE validates the builder output before applying it,
so a failed script leaves the previous generated geometry untouched.

## Quick start

```python
# Declared parameters are strings; convert them with layout.um/length/float.
width = layout.um(layout.parameter("width", "4", "Metal width in micrometers"))

metal1 = layout.layer("Metal1", 1)
metal1.rectangle(0, 0, width, layout.um(0.6), name="M1", net="VDD")
metal1.rectangle(0, layout.um(2.4), width, layout.um(0.6), name="M2", net="VSS")

layout.port(
    "VDD", "Metal1", 0, 0, width, layout.um(0.6),
    direction="power", side="left", net="VDD",
)
```

Create and regenerate the layout from the GUI (**+** in the Layout
**Hierarchy** → **Procedural Layout**), the CLI, `icepy`, or MCP. The generated
child is read-only; edit the script or parameter form and regenerate instead of
drawing over it.

## Discover and validate the live API

Do not guess the script API. The MCP tools `layout_api` and
`layout_validate_script` are generated from the running Python runner:

```text
layout_api(pdk="sky130A")                 # classes + key methods + device names
layout_api(name="device")                 # exact signature, parameters, example
layout_api(class_name="Layout")           # every public Layout method
layout_api(query="route")                 # search methods, parameters, devices
layout_validate_script(script=..., pdk="sky130A", layout="ota")
```

`layout_api` reports `api_version`, public classes, method signatures,
parameter descriptions, return types, and minimal examples. When `pdk` is
given it also lists the PDK's layout device names, so a script can use the
right cell name (for example `nfet` instead of a guessed `nmos`).

`layout_validate_script` parses the Python AST without executing package code.
It reports Python syntax errors, unknown `layout.*` methods, wrong keywords or
argument counts, missing `layout.plan(...)` or
`layout.finish_placement()`, `layout.add(... w=..., l=...)` misuse, and planned
devices that are never created or never added. It is advisory, but call it
before `layout_regenerate` and fix every error: a lint error is much cheaper
than a failed regeneration.

| Entry point | Create | Regenerate |
| --- | --- | --- |
| GUI | Layout **Hierarchy** → **+** → **Procedural Layout** | **Regenerate layout** button or context menu |
| CLI | `ice-cli layout-add-procedural <project> <name> [pdk] [script-file] [stack]` | `ice-cli layout-regenerate <project> <procedural> [timeout-seconds]` |
| `icepy` | `project.create_procedural_layout(...)` | `project.regenerate_procedural_layout(name)` |
| MCP | `layout_create_procedural` | `layout_regenerate` |

The GUI runs scripts with a 60-second timeout. The CLI and MCP regeneration
operations accept a timeout override. Set `ICE_PYTHON` to choose the
interpreter; otherwise `python3` is used.

## Coordinates and units

Geometry methods take integer database units. A new project normally uses a
1 nm database unit, so an explicit number is a nanometer count. Prefer the
unit helpers so scripts remain readable and adapt to the project unit:

| Helper | Meaning |
| --- | --- |
| `layout.db(value)` | Round a value to integer database units. |
| `layout.nm(value)` | Convert nanometers to database units. |
| `layout.um(value)` | Convert micrometers to database units. |
| `layout.snap(value)` | Snap a database-unit value to the project grid. |
| `layout.length(value)` | Parse a suffix such as `2.5u`, `150n`, or `1mm`. |

The builder also exposes `layout.grid` (snap grid in database units) and
`layout.db_unit_nm` (database unit size). All emitted geometry is rounded to
integer database units.

## Layers

`layout.layer(name, number=None, datatype=None)` registers or resolves a layer
and returns a reusable {py:class}`procedural_runner.Layer` handle. When the
stream values are omitted, ICE resolves the layer from the active PDK package
or from layers already present in the project. Explicit values are used as a
fallback.

Purpose-specific layers keep their own datatype. For example, a pin layer can
be declared independently of its drawing layer:

```python
m1 = layout.layer("Metal1")
m1_pin = layout.layer("Metal1.pin", 8, 2)

m1.rectangle(0, 0, 4000, 600)
m1_pin.rectangle(0, 0, 400, 600)
```

The same calls are available as `layout.rectangle("Metal1", ...)` and
`layout.polygon("Metal1", ...)` when a handle is not convenient. `Layout.rect`
and `Layout.poly` are aliases for `Layout.rectangle` and `Layout.polygon`;
`Layer.rect` and `Layer.poly` are the matching layer-handle aliases.

## Geometry

Rectangle, polygon, path, text, and boolean methods return a
{py:class}`procedural_runner.Shape` handle whose `name` and `net` attributes
can be edited after creation. The `net` is used by connectivity checks and by
the autorouter to distinguish connectable geometry from obstacles.

### Rectangles, polygons, and holes

```python
metal = layout.layer("Metal2")

metal.rectangle(0, 0, 4000, 600, name="body", net="OUT")
metal.polygon(
    [(0, 1000), (4000, 1000), (4000, 3000), (2000, 4000), (0, 3000)],
    holes=[[(1000, 1500), (3000, 1500), (3000, 2500), (1000, 2500)]],
    name="shield",
    net="VSS",
)
```

Polygon points may be `(x, y)` tuples or mappings with `x` and `y` keys.
Widths and heights must be positive; invalid geometry raises an error and
aborts the run.

### Paths and text

```python
metal.path([(0, 0), (4000, 0), (4000, 2000)], width=layout.um(0.5), net="VDD")
metal.text(0, 2500, "M2", height=layout.um(0.5), net="VDD")
```

A path stores its centerline and width; ICE expands it to polygons for display,
bounds, and `layout.iter_polygons()`. Text is stored as a bounding rectangle
with text metadata. When `width` is omitted, the text width is estimated from
the label length and height.

### Boolean operations

`layout.boolean(operation, layer, *operands)` combines shapes, rectangles, or
point lists on the output layer. The helpers `union`, `intersection`,
`subtract`, and `xor` are convenience wrappers:

```python
left = metal.rectangle(0, 0, 3000, 1000)
right = metal.rectangle(1000, 500, 3000, 1000)
merged = layout.union("Metal2", left, right, name="merged", net="VDD")
```

Operands may be consumed by the operation, so create fresh shapes when the
original geometry is still needed.

### Inspection helpers

PDK extensions such as dummy fill can inspect the completed builder:

- `layout.iter_polygons(layer=None)` yields `(layer_name, [(x, y), ...])` for
  rectangles, polygons, and expanded path segments.
- `layout.bounds(layer=None)` returns `(min_x, min_y, max_x, max_y)` or `None`
  when no geometry matches. Without a layer filter it includes ports and child
  instances; pass a layer name to restrict the result to shapes on that layer.
- `layout.cells` is the project layout-cell catalog keyed by cell name. Each
  entry carries the cell `bounds` and `ports` when ICE supplied the catalog.
- `layout.layer_number(name)` returns the registered GDS layer number.

## Ports

`layout.port(...)` adds an intrinsic ICE port to the generated cell. Ports
participate in hierarchy wiring, connectivity checks, and LVS. ICE uses
**port** for a layout-cell terminal and **pin** for a schematic-symbol
terminal; LEF/DEF keep their `PIN` keyword at the file-format boundary.

```python
layout.port(
    "OUT",
    "Metal1",
    3000,
    0,
    1000,
    600,
    direction="output",
    side="right",
    net="OUT",
    label="OUT",
)
```

The layer argument is required. It is stored as the port's layer association
and is used by the router. It does not make the port process-layer geometry,
and the named layer may be a routing layer rather than a PDK pin purpose. The
port rectangle, name, direction, side, label, net, and layer are saved in the
generated cell's ports.

- `direction` is `input`, `output`, `inout`, `power`, or `ground`.
- `side` is `left`, `right`, `top`, or `bottom`.
- `label` defaults to the port name.
- `visible` and `locked` control editor behavior.
- `net` binds the port to a layout net.

## Instances and arrays

Use `layout.instance(cell, x, y, orientation="r0", name=None)` to place an
existing layout cell by name. The generated cell references the target cell;
geometry is not copied. Targets may be static layout cells, other generated
cells, imported digital devices, or package fixed-device cells.

`layout.instance(...)` returns a `LayoutInstancePlacement` handle. It reports
the referenced cell's real `bounds`, `width`, `height`, `ports`, and
`port_info`, and it can be moved, mirrored, added to rows/columns/grids, or used
as a routing endpoint:

```python
block = layout.instance("top_impl device", 0, 0, name="DIGITAL")
pad = layout.layer("Metal1", 34).rectangle(0, 0, 100, 100)

row = layout.row(gap=0)
row.add(pad)
row.add(block)          # packed using the digital cell's real width
layout.add(row)

print(block.bounds)     # {"x": ..., "y": ..., "width": ..., "height": ...}
print(block.ports["clk"])
layout.connect(device, "G", block, "clk", layer="Metal1")
```

Digital devices imported from an implementation run
(`implementation-import` or `implementation_import_layout`) are read-only
layout cells. The import copies the top-level DEF pins, directions, and pin
nets onto the flattened device, so `layout.instance(...).ports` and
`layout.connect(...)` can route to them. Use the cell name reported by the
import, for example `"top_impl device"`.

`layout.array(cell, count, pitch, ...)` places a rectangular array and returns
the instance names. `count` and `pitch` are `(columns, rows)` and
`(x_pitch, y_pitch)`.

Supported instance orientations are `r0`, `r90`, `r180`, `r270`, `mx`, `my`,
`m45`, and `m135`. Device placement supports `r0`, `r90`, `r180`, `r270`, `mx`,
and `my`.

## Parameters

Declare a parameter with `layout.parameter(name, default, description)`. The
method returns the current value as a string, using the default only when the
project has no stored value.

```python
length = layout.um(layout.parameter("length", "10", "Channel length in um"))
fingers = int(layout.parameter("nf", "2", "Number of fingers"))
```

Declared parameters appear in the GUI parameter form above the script editor.
They can also be changed before regeneration through:

- CLI: `ice-cli layout-set-parameter <project> <procedural> <name> <value>`
- `icepy`: `project.set_procedural_parameter(layout_name, parameter_name, value)`
- MCP: `layout_set_parameter`

A procedural layout reports `needs_regeneration` when its script hash or
parameter hash differs from the last successful run.

## Devices

`layout.device(...)` places a device from the active package's layout device
library. The library may contain generated PCells and fixed `.icelay` cells.
Device geometry is flattened into the generated cell, while a planned-device
record preserves the model, parameters, and terminal nets for LVS and PEX.

```python
m1 = layout.device(
    "nfet", name="M1", x=0, y=0,
    w="2", l="0.15", nf="1",
    nets={"G": "GATE", "D": "OUT", "S": "VSS", "B": "VSS"},
)
m2 = layout.device(
    "nfet", name="M2", x=layout.um(12), y=0,
    w="4", l="0.15", nf="1",
    nets={"G": "GATE", "D": "OUT", "S": "VSS", "B": "VSS"},
)
```

The returned {py:class}`procedural_runner.DevicePlacement` exposes
absolute port rectangles as attributes (`m1.G`) and as a mapping
(`m1.ports["G"]`). It also reports `device`, `model`, `parameters`, `bounds`,
and per-port `port_info`. Emitted device ports are prefixed with the instance
name and marked internal, so multiple copies can coexist.

Device parameters are passed as keyword strings. Matching is case-insensitive
and falls back to a substring match against the library index. A missing
package library, unknown device, or failed PCell generator raises an error.

## Composition

Rows, columns, grids, stacks, and repeats compose existing handles without
manual coordinate arithmetic. Each composition is itself a handle with
`bounds`, `ports`, `anchor`, `move`, and `mirror`, so compositions can be
nested and placed into a group or the root builder.

```python
pair = layout.group("input_pair")
m1 = pair.device("nfet", name="M1", w="2", l="0.15",
                 nets={"G": "IN", "D": "OUTN", "S": "VSS"})
m2 = pair.device("nfet", name="M2", w="2", l="0.15",
                 nets={"G": "IN", "D": "OUTP", "S": "VSS"})
m2.mirror("y")

core = layout.row(gap=0, align="bottom")
core.add(m1)
core.add(m2)
core.add(layout.spacer(layout.um(0.5)))
core.add(layout.generate("rail", width=layout.um(8), height=layout.um(0.4)))
layout.add(core, x=layout.um(2), y=layout.um(1))
```

| Method | Purpose |
| --- | --- |
| `layout.group(name=None)` | Local-coordinate fragment with the full builder API. |
| `layout.row(gap, align, distribute, width)` | Horizontal composition. |
| `layout.column(gap, align, distribute, height)` | Vertical composition. |
| `layout.grid(columns, x_gap, y_gap, align)` | Fixed-column grid. `layout.grid` remains the integer snap grid, but is callable for grid composition. |
| `layout.stack(items, align)` | Overlay items on a common origin. |
| `layout.repeat(unit, count, axis, pitch, mirror, alternate)` | Repeated, optionally mirrored copies. |
| `layout.spacer(distance)` | Fixed gap in a row or column. |
| `layout.stretch(weight=1)` | Flexible gap that consumes slack. |
| `layout.add(item, x, y, mirror, prefix)` | Copy a composition into the current builder. |

Row and column alignment accepts `top`, `bottom`, `center`, `left`, and
`right`. `distribute="space-between"` or `"equal"` distributes slack when an
explicit `width` or `height` is given; `layout.stretch()` participates in the
same slack distribution. `repeat(..., pitch="abut")` places copies edge to
edge, and `mirror=True, alternate=True` produces alternating mirrored copies
for interdigitated structures.

Composition copies groups and repetitions, but consumes concrete shapes,
devices, and routes: adding `m1` to a row removes `m1`'s records from its
previous builder while keeping the `m1` handle valid. Port names from repeated
copies are suffixed (`P`, `P_2`, `P_3`, ...). Use `layout.add(..., prefix=...)`
when embedding a fragment with names that already exist.

### Inspection and measurement

```python
layout.extents([m1, m2])       # union bounds or None
layout.clearance(m1, m2)       # edge-to-edge gap; negative when overlapping
layout.overlaps(m1, m2)
layout.contains(frame, core)
layout.distance(m1, m2, axis="x")
layout.measure(core)           # bounds, width, height, area
```

Every handle also exposes `bounds`, `width`, `height`, `ports`, `port(name)`,
`anchor(name)`, `move(dx, dy)`, `mirror(axis)`, and `is_empty`. Composition and
shape bounds are `(x0, y0, x1, y1)` tuples; `DevicePlacement.bounds` keeps the
existing `x`/`y`/`width`/`height` mapping for router compatibility.
`mirror(axis="y")` flips horizontally; use `axis="x"` to flip vertically.

## Constraints

Constraints express local placement intent that is awkward to encode as
nested rows and columns. They resolve deterministically and modify the involved
records in place:

```python
layout.constraint("Order", items=[m1, m2, m3],
                  direction="left_to_right", abut=True)
layout.constraint("Align", items=[m1, m2, m3], line="h_center")
layout.constraint("Symmetric", pairs=[[m1, m2]], axis="vertical")
layout.constraint("Spread", items=[m1, m2, m3],
                  direction="horizontal", distance=layout.um(0.4))
layout.constraint("Boundary", max_width=layout.um(60), max_height=layout.um(40))
layout.constraint("PortLocation", port="IN", side="left", align="center")
```

Convenience wrappers are available for the common cases: `layout.align`,
`layout.order`, `layout.abut`, `layout.symmetric`, `layout.distribute`, and
`layout.boundary`. Supported constraints are `Align`, `AlignInOrder`, `Order`,
`Symmetric`, `Group`, `Boundary`, `AspectRatio`, `Spread`, `Grid`,
`PortLocation`, and `Generator`. Invalid handles, unknown ports, and
unsatisfiable boundaries or aspect ratios raise an error before geometry is
committed.

## Cell generators

A generator is a reusable function that builds a local group. Declare an
inline generator with `@layout.generator` and instantiate it with
`layout.generate`:

```python
@layout.generator(
    name="nmos_pair",
    description="Matched NMOS pair",
    parameters={"device": "nfet", "w": "2", "l": "0.15"},
    ports=["D1", "G1", "S", "D2", "G2", "B"],
)
def nmos_pair(ctx, device, w, l):
    row = ctx.row(gap=0, align="center")
    left = ctx.device(device, name="M1", w=w, l=l, nets={"S": "VSS"})
    right = ctx.device(device, name="M2", w=w, l=l, nets={"S": "VSS"})
    right.mirror("y")
    row.add(left)
    row.add(right)
    return row

pair = layout.generate("nmos_pair", w="4")
layout.add(pair)
```

Built-in generators are available without a package: `mos`, `mos_pair`,
`resistor`, `resistor_bank`, `cap`, `cap_array`, `via_stack`, `guard_ring`,
`rail`, and `route_bundle`. Device-based built-ins accept the package `device`
name and pass remaining parameters to `layout.device`. Packages may provide
additional generators under `<pdk>/layout/generators/`; a package entry with
the same name replaces the built-in one. Generator metadata is reported in the
generated JSON for GUI, CLI, and MCP discovery. `python3
assets/layout/procedural_runner.py --list-generators` prints the built-in
metadata as JSON.

## Area quality check

A layout that routes correctly can still waste most of its area. Start composed
rows and columns with `gap=0` and add spacing only where routing or DRC needs
it. Prefer one shared routing channel over fixed padding between every device.

### Planning and the compaction gate

Declare a plan before placing devices:

```python
layout.plan(
    devices=["M1", "M2", "M3", "M4"],
    rows=2,
    target_utilization=0.25,
    max_gap_um=10,
    require_composition=True,
)

# Place and compact the devices.
layout.finish_placement()   # or layout.compact()
```

`layout.finish_placement()` runs the quality report and verifies the declared
device set, row count, utilization, maximum row gap, and composition
requirements. When the plan sets `require_gate=True` (the default), `route()`
and `connect()` are blocked until the gate passes. A failed gate raises an
error with the failing metrics; compact the placement and call
`finish_placement()` again.

### Quality report

Call `layout.report()` after placement and routing:

```python
layout.report(print_report=True)
```

The report includes:

- `cell_area_um2`, `device_area_um2`, `instance_area_um2`,
  `occupied_area_um2`, and `utilization`;
- `device_utilization` for device-only area, and macro-aware `utilization`
  that includes placed child instances;
- row-grouped `rows`, `row_count`, `max_horizontal_gap_um`, and `large_gaps`;
- `composition_used`, `placement_style`, `manual_move_count`, and
  `composition` counters;
- `plan_present`, `plan_passed`, shape, port, device, and instance counts;
- warnings for low utilization and large gaps inside one row.

The plan gate and review use `utilization`, so an imported digital block
counts toward the area budget even when the layout has no planned devices.

Devices are grouped into row bands by vertical center and similar height before
gaps are measured. This keeps tall passives such as MIM capacitors from being
compared with every small transistor in the cell.

### Separate review

After routing and verification, run the independent review step:

```python
layout.review(
    min_utilization=0.25,
    max_gap_um=10,
    require_composition=True,
    require_plan=True,
)
```

`review()` returns `passed`, `issues`, the placement `quality`, the final
`cell_quality`, and the declared `plan`. It is also exposed as the MCP tool
`layout_review` and the CLI command `layout-review`.

The generated JSON now carries `layout_quality`, `layout_plan`, and
`layout_review`. MCP `layout_regenerate` and CLI `layout-regenerate` return those
objects directly, so agents do not need to parse the generation log to see
whether the compaction gate passed. A good first target is utilization above
roughly 20–25% with no unexplained row gap above roughly 10 µm.

## Routing

The builder includes a deterministic Manhattan router for both points and
placed device or layout-instance ports.

### Point and rectangle routing

```python
route = layout.route(
    (0, 0),
    (layout.um(20), 0),
    layer="Metal1",
    width=layout.um(0.4),
    net="VDD",
)
print(route.length, route.bends, route.via_count)
```

`start` and `end` may be `(x, y)` points, `(x, y, width, height)` port
rectangles, or port tuples from a `DevicePlacement` or
`LayoutInstancePlacement`. Rectangle endpoints escape from `start_side` /
`end_side` (or the side nearest the other endpoint) by `escape`. The router
avoids placed device and instance bounds and other-net shapes by default and
raises an error rather than emitting a short when no legal path exists.

### Connecting device and instance ports

```python
layout.connect(m1, "G", m2, "G", layer="Metal1", net="GATE")
layout.connect(m1, "D", digital, "out", layer="Metal1", net="OUT")
```

`connect(device_a, pin_a, device_b, pin_b, ...)` reads port layers, sides, and
nets from the placements. Same-layer ports can use `layer`; ports on different
layers require `layers=[...]` in vertical stack order. The router inserts via
stacks between adjacent layers when the package exposes via metadata.

### Multi-layer routing and vias

```python
route = layout.route(
    m2.D,
    (layout.um(20), layout.um(2), layout.um(1), layout.um(0.6)),
    layers=["Metal1", "Metal2"],
    width=layout.um(0.4),
    net="OUT",
)
```

When `layer` and `layers` are both omitted, `layout.routing_stack()` supplies
the package's declared stack. `technology/connectivity.json` declares layer
roles, and via rules provide the cut layer, size, and enclosure. The router
falls back to common names such as `via`/`via2`, `Via1`/`Via2`, and
`Metal1`/`Metal2` when no explicit mapping is available. Use `via_layers` to
override the mapping, and `via_size`, `via_enclosure`, and `via_cost` to tune
transition geometry and cost.

Additional routing controls:

| Argument | Effect |
| --- | --- |
| `width` | Trace width; default 0.4 µm. |
| `clearance` | Edge-to-edge clearance; default `max(width, 0.2 µm)`. |
| `escape` | Port escape distance; default `max(2 * width, 0.5 µm)`. |
| `obstacles` | Extra rectangles, a list, or per-layer obstacle mappings. |
| `bend_penalty` | Extra cost per bend; default `max(2 * width, 1 µm)`. |
| `include_devices` | Treat other device bounds as obstacles. |
| `include_shapes` | Treat other-net layout shapes as obstacles. |
| `start_layer` / `end_layer` | Explicit endpoints in a layer stack. |

The returned {py:class}`procedural_runner.Route` reports `net`, `layer`,
`width`, `points`, `point_layers`, `segments`, `vias`, `shapes`, `length`,
`bends`, `layer_stack`, and `via_count`. Its metal and via shapes are already
part of the generated layout.

## PDK-provided operations

Procedural scripts can request process-specific work through generic builder
calls. ICE dispatches the request to the active package after the user script
finishes.

### Dummy fill

```python
layout.add_dummy_fill(layers=["Metal1", "Metal2"], density=50, deviation=10)
```

The package implementation lives at `layout/dummy_fill.py` and receives the
completed builder. The IHP SG13G2 package implements dummy metal fill for its
stack. A package without the implementation reports a clear error. See
{doc}`/developer/packages` for the package contract and routing connectivity
schema.

### Routing stack and device library

The router consults `technology/connectivity.json` for the declared vertical
stack and via rules. Device placement consults
`layout/devices/index.json`, generated PCells, and fixed device cells. Both
are package-owned, so switching the active layout to another PDK switches the
available devices and routing metadata.

## Execution and validation

A run is isolated from the project database:

1. ICE writes the script and parameter JSON to a temporary directory.
2. Python executes the script with the `layout` builder.
3. The builder emits a versioned JSON document with layers, parameters,
   shapes, ports, instances, and devices.
4. ICE parses and validates the document, then applies it to the project.
5. The generated child replaces its previous contents; its script/output
   hashes and stale state are updated.

If the script fails, times out, emits no output, or produces invalid geometry,
the run reports the Python error or validation message and leaves the previous
generated cell unchanged. The generated cell is read-only and is never a place
for manual edits.

## API reference

The reference below is generated by Sphinx autodoc from the public docstrings
in `assets/layout/procedural_runner.py`.

```{eval-rst}
.. automodule:: procedural_runner
   :no-members:
```

```{eval-rst}
.. autoclass:: procedural_runner.Layout
   :members:
   :exclude-members: rect, poly
```

```{eval-rst}
.. autoclass:: procedural_runner.Layer
   :members:
   :exclude-members: rect, poly
```

```{eval-rst}
.. autoclass:: procedural_runner.Shape
   :members:
```

```{eval-rst}
.. autoclass:: procedural_runner.DevicePlacement
   :members:
```

```{eval-rst}
.. autoclass:: procedural_runner.LayoutInstancePlacement
   :members:
```

```{eval-rst}
.. autoclass:: procedural_runner.Route
   :members:
```

```{eval-rst}
.. autoclass:: procedural_runner.Group
   :members:
```

```{eval-rst}
.. autoclass:: procedural_runner.Row
   :members:
```

```{eval-rst}
.. autoclass:: procedural_runner.Column
   :members:
```

```{eval-rst}
.. autoclass:: procedural_runner.Grid
   :members:
```

```{eval-rst}
.. autoclass:: procedural_runner.Stack
   :members:
```

```{eval-rst}
.. autoclass:: procedural_runner.Repeat
   :members:
```

## See also

- {doc}`gui` — procedural layout editor and regeneration workflow.
- {doc}`icepy` — creating and regenerating procedural layouts from Python.
- {doc}`cli` — headless procedural layout commands.
- {doc}`ice-mcp` — MCP tools for scripts, parameters, and regeneration.
- {doc}`/developer/packages` — device libraries, connectivity, and dummy fill
  contracts.
