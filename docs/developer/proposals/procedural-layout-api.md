# Procedural Layout API: Constraint-Aware Generators and Composition

**Status:** Implemented for phases 1–3 plus package generator loading; ALIGN
adapter and a general solver remain future work
**Audience:** `openvlsilib`, `openvlsipy`, GUI, CLI, MCP, and package-generator contributors
**Scope:** the Python procedural-layout builder and the native OpenVLSI layout cells it
produces

## 1. Summary

OpenVLSI procedural layouts are currently built from low-level calls:
`layout.rectangle`, `layout.polygon`, `layout.device`, `layout.route`, and so
on. This is flexible, but every device pitch, alignment, mirroring, shared rail,
and routing corridor must be calculated by the script author. Reusable layout
knowledge is copied between scripts rather than captured as a generator.

This proposal adds three layers on top of the existing builder:

1. **Composable layout objects** — rows, columns, grids, stacks, repeated and
   mirrored elements, with explicit bounds, ports, and pin anchors.
2. **Cell generators** — parameterized, reusable Python builders that produce a
   complete local layout fragment with declared parameters and ports.
3. **Lightweight layout constraints** — deterministic alignment, ordering,
   symmetry, spacing, boundary, aspect-ratio, and port-placement intent.

The result should make common analog layout structures easier to express and
more space-efficient through abutment, shared rails, matching, and
route-aware placement, while preserving the existing low-level API as an escape
hatch.

This is **not** an ALIGN integration project. ALIGN is used only as a source of
well-tested terminology and constraint ideas. No ALIGN code, schema, solver, or
runtime will be added by this proposal.

## 2. Goals

- Make repeated device, resistor, capacitor, via, and guard-ring structures
  reusable.
- Make rows, columns, arrays, and matched pairs first-class objects rather than
  hand-computed coordinate lists.
- Reduce unused area through:
  - abutment;
  - shared power/ground rails;
  - common-centroid and interdigitated device patterns;
  - port-to-port alignment;
  - route-aware placement;
  - bounded compaction within rows and columns.
- Make layout intent inspectable and testable before geometry is emitted.
- Keep generated results as ordinary native OpenVLSI layout cells so DRC, LVS, PEX,
  GDS export, hierarchy, and simulation continue to work unchanged.
- Keep existing procedural scripts source-compatible.

## 3. Non-goals

- Do not run, embed, or depend on ALIGN.
- Do not implement a full analog placement or routing solver in this work.
- Do not replace explicit geometry APIs.
- Do not make a generic global floorplanner.
- Do not make procedural generation non-deterministic or dependent on external
  package installation.
- Do not change the meaning of existing `layout.rectangle`, `layout.polygon`,
  `layout.device`, or `layout.route` calls.

## 4. Current baseline

The existing procedural runner already provides:

- integer database-unit layout construction;
- layer resolution;
- rectangles, polygons, paths, text, and Boolean operations;
- ports;
- device placement from PDK PCells;
- point and pin routing with vias;
- parameter declarations and hashed regeneration;
- generated static cells that are readable by the rest of OpenVLSI.

The missing layer is **composition and intent**. Today, a script that wants a
matched pair, a resistor bank, or a common rail must manually:

- find each device's absolute bounds;
- calculate pitch and mirroring;
- offset every device, port, and route;
- decide where to place decoupling cells;
- keep ports aligned to the eventual routing direction.

This proposal moves that knowledge into reusable builders and constraints.

## 5. Design principles

1. **Local coordinates, final transforms.** Generated fragments are described
   in local coordinates and placed, mirrored, or repeated as handles. Geometry
   is emitted only after composition is resolved.
2. **Deterministic.** Given the same script, parameters, generator versions,
   and PDK, geometry and hashes are identical.
3. **Builder-first.** Everything ultimately compiles to the existing
   `procedural_runner` shape, device, port, and route records.
4. **Explicit escape hatch.** Manual geometry remains available inside any
   generated fragment.
5. **No hidden dependency.** Built-in generators work without a PDK package.
   Package generators are versioned and optional.
6. **Measurable output.** Every layout object exposes bounds, ports, pins, and
   clearance/overlap queries so scripts can make decisions without duplicating
   geometry math.
7. **Readable errors.** Invalid constraints and unsatisfiable layouts report
   the objects and constraint involved.

## 6. API overview

```text
layout
├── low-level geometry (existing)
├── layout.group(...)             # local-coordinate fragment
├── layout.row(...)               # horizontal composition
├── layout.column(...)            # vertical composition
├── layout.grid(...)              # fixed rows/columns
├── layout.stack(...)             # overlap on the same origin
├── layout.repeat(...)            # repeated/mirrored copies
├── layout.spacer(...)            # fixed or stretchable gap
├── layout.generate(...)          # named generator
├── layout.constraint(...)        # declarative relation
├── layout.align(...)             # convenience wrappers
├── layout.order(...)
├── layout.abut(...)
├── layout.symmetric(...)
└── layout.measure(...)           # extents, clearance, overlap
```

### 6.1 Layout handles

Every geometry-producing operation returns a handle with a common interface:

```python
handle.bounds          # (x0, y0, x1, y1) in local or final coordinates
handle.width
handle.height
handle.ports           # name -> PortRef
handle.pin("D")        # device terminal, if present
handle.anchor("c")     # named anchor point
handle.move(dx, dy)
handle.mirror(axis="y")
handle.is_empty
```

Handles must remain valid after composition. A child can be queried for its
bounds or a port before its final offset is applied. The runner can update a
handle's deferred transform when it is added to a row, column, or grid.

Existing `Shape` and `DevicePlacement` objects are wrapped by the same handle
protocol. For compatibility, existing immediate calls keep their current
behavior; transforms are supported for objects created inside a `group` or
generator.

### 6.2 Groups

A group records geometry in local coordinates and can be placed as a unit:

```python
pair = layout.group("input_pair")
m1 = pair.device("nfet", x=0, y=0, w="2u", l="0.15u", nf=2, nets=...)
m2 = pair.device("nfet", x=pair.um(1.2), y=0, w="2u", l="0.15u", nf=2, nets=...)
pair.port("IN", "Metal1.pin", ...)
pair.port("OUT", "Metal1.pin", ...)
```

Groups:

- expose bounds and ports;
- can contain shapes, devices, instances, routes, and nested groups;
- can be mirrored and repeated;
- can be assigned a name for diagnostics and generated-cell reuse.

A group is not necessarily a separate static layout cell. It is a local
fragment by default. A later option can materialize frequently reused groups as
generated child cells.

### 6.3 Row and column composition

```python
row = layout.row(
    gap=0,
    align="bottom",       # top | bottom | center | baseline
    distribute="fixed",   # fixed | space-between | equal
)

row.add(m1)
row.add(m2)
row.add(layout.spacer(200))
row.add(bank)
```

```python
column = layout.column(gap=layout.um(0.4), align="left")
column.add(row)
column.add(bias_row)
```

Properties:

- `gap` is a minimum gap in database units or a unit expression such as
  `layout.um(0.4)`.
- `gap=0` enables abutment where bounds permit it.
- `align` aligns child bounds or selected anchors.
- `distribute="space-between"` expands gaps to fill an explicit container
  width.
- `distribute="equal"` spreads children with equal gaps.
- A row/column computes its extents after all children are added. Adding a
  child that changes alignment or makes a constraint unsatisfiable is reported
  with the child and row names.

Rows and columns can be nested:

```python
left = layout.column(gap=0)
left.add(mirror_pair)

right = layout.column(gap=layout.um(0.3))
right.add(bias)

core = layout.row(gap=layout.um(1.0), align="center")
core.add(left)
core.add(right)
```

### 6.4 Grid, stack, and repeat

```python
bank = layout.grid(
    items=[r1, r2, r3, r4],
    columns=2,
    x_gap=0,
    y_gap=layout.um(0.4),
    align="center",
)
```

```python
power = layout.stack([vdd_ring, core, vss_ring])
```

```python
interdigitated = layout.repeat(
    unit,
    count=6,
    axis="x",
    pitch="abut",
    mirror=True,
    alternate=True,
)
```

Semantics:

- `grid` lays out rows of children with explicit column count and gaps.
- `stack` overlays children on the same origin, primarily for rings, rails, and
  markers; it does not merge geometry.
- `repeat` copies a handle along an axis. `pitch` is a distance, `"abut"`;
  `mirror` and `alternate` control common-centroid/interdigitated styles.
- Repeated copies may be instances of a generated child cell when the group is
  large enough to justify hierarchy.

### 6.5 Spacers and stretch

```python
row.add(layout.spacer(300))
row.add(layout.stretch())
```

- `spacer(distance)` reserves a fixed gap.
- `stretch()` consumes remaining space when the parent uses
  `distribute="space-between"` or an explicit width.
- Stretch objects are useful for keeping ports at the cell boundary while
  packing devices around a shared rail.

## 7. Cell generators

### 7.1 Generator protocol

A cell generator is a Python function or class that declares parameters and
ports and builds a local group:

```python
@layout.generator(
    name="nmos_pair",
    description="Matched NMOS pair with shared source and optional guard ring",
    parameters={
        "w": "2u",
        "l": "0.15u",
        "nf": 2,
        "m": 1,
        "pattern": "cc",       # single | id | cc
        "shared_source": True,
        "guard_ring": False,
    },
    ports=["D1", "G1", "S", "D2", "G2", "B"],
)
def nmos_pair(ctx, w, l, nf, m, pattern, shared_source, guard_ring):
    pair = ctx.group("nmos_pair")
    ...
    return pair
```

Usage:

```python
pair = layout.generate(
    "nmos_pair",
    w="4u",
    l="0.15u",
    nf=4,
    pattern="cc",
    shared_source=True,
)
row.add(pair)
```

Generator properties:

- Parameters are typed, defaulted, and validated before geometry is emitted.
- Ports have name, layer, direction, side, and net intent.
- Generated groups expose bounds and port anchors for composition.
- Generator output is deterministic and cacheable.
- A generator may call other generators.
- A generator may use `group.device`, `group.rectangle`, `group.route`, and
  explicit coordinates.

### 7.2 Generator sources

Proposed resolution order:

1. Built-in OpenVLSI generators under `assets/layout/generators/`.
2. Project generators under `generators/` (new optional project folder).
3. PDK-package generators under `<pdk>/layout/generators/`.
4. Inline generators declared in the current script with
   `@layout.generator`.

Inline generators are the smallest step and can ship first. Project and
package registries follow once the API is stable.

Each registry entry should have:

```json
{
  "name": "nmos_pair",
  "version": 1,
  "module": "generators/analog.py",
  "class": "NmosPair",
  "category": "Matching",
  "parameters": ["w", "l", "nf", "m", "pattern"],
  "ports": ["D1", "G1", "S", "D2", "G2", "B"]
}
```

Package generator code runs through the same external Python process and
timeout model as procedural layouts. It must not receive unrestricted access to
the project database.

### 7.3 Built-in generators

The first useful set:

| Generator | Purpose |
| --- | --- |
| `mos` | Single transistor / finger bank |
| `mos_pair` | Matched or mirrored MOS pair |
| `resistor` | Single resistor |
| `resistor_bank` | Series/parallel/interdigitated resistor bank |
| `cap` | Single capacitor |
| `cap_array` | Common-centroid or unit-cap array |
| `via_stack` | Layer-to-layer via stack |
| `guard_ring` | Ring around a group or point |
| `rail` | Power/ground rail with ports |
| `spacer` | Layout spacer |
| `route_bundle` | Repeated route corridor |

Device-level generators should reuse the existing package PCell system rather
than duplicate device geometry calculations.

### 7.4 Generator metadata and GUI

The Python runner should report generator descriptions, parameters, port
anchors, bounds, and diagnostics. This metadata can power:

- GUI parameter forms;
- a generator palette;
- CLI/MCP discovery;
- regeneration hashes;
- documentation.

Generator metadata must not be required for a script to run. A plain
procedural script remains valid.

## 8. Constraints

Constraints capture relationships that are awkward to express as nested rows
and columns. They are **local** to the current procedural cell or generator.

### 8.1 Constraint forms

```python
layout.constraint("Align", items=[m1, m2, m3], line="h_center")
layout.constraint("Order", items=[m1, m2], direction="left_to_right", abut=True)
layout.constraint("Symmetric", pairs=[[m1, m2]], axis="vertical")
layout.constraint("Group", items=[m1, m2], name="input_pair")
layout.constraint("Boundary", max_width=layout.um(60), max_height=layout.um(40))
layout.constraint("AspectRatio", minimum=0.5, maximum=2.0)
layout.constraint("Spread", items=[m1, m2, m3], direction="horizontal", distance=300)
layout.constraint("PortLocation", port="IN", side="left", align="center")
layout.constraint("Grid", pitch=layout.um(0.1))
layout.constraint("Generator", item=pair, options={"pattern": "cc"})
```

Convenience wrappers:

```python
layout.align(m1, m2, line="h_center")
layout.order([m1, m2, m3], direction="left_to_right", abut=True)
layout.abut(m1, m2, edge="right")
layout.symmetric([m1, m2], axis="v")
layout.distribute([m1, m2, m3], axis="x", mode="equal")
layout.boundary(max_width=..., max_height=...)
```

### 8.2 Supported constraints

#### Placement and order

- `Order`: left-to-right, right-to-left, bottom-to-top, top-to-bottom,
  optional abutment.
- `Align`: top, bottom, center, left, right, or any contained edge.
- `AlignInOrder`: convenience composition of align and order.
- `Spread`: minimum spacing when items overlap in the orthogonal direction.
- `Group`: treat several items as one composition block.
- `Boundary`: maximum width/height plus optional halo.
- `AspectRatio`: minimum and maximum width/height ratio.
- `Grid`: snap generated geometry to a project/pitch grid.
- `PortLocation`: preferred side/alignment for a port.

#### Matching and symmetry

- `Symmetric`: vertical/horizontal symmetry pairs.
- `SymmetricNets`: corresponding device terminals/nets.
- `CommonCentroid`: generator-level pattern hint for matched devices.
- `Interdigitated`: repeated alternating/mirrored fingers.

#### Generator intent

- `Generator`: generator-specific options such as `pattern`, `shared_diff`,
  `shared_source`, `body`, `height`, `fingers`, and guard-ring options.

#### Later additions

- `GuardRing` constraints.
- `Route` and `DoNotRoute` corridors.
- `NetPriority`.
- `ChargeFlow`.
- `MultiConnection`.

These later constraints should be added only after the native placement and
routing semantics are well defined.

### 8.3 Constraint resolution

The runner resolves constraints deterministically in this order:

```text
1. Build each group in local coordinates
2. Place explicit rows, columns, grids, and stacks
3. Apply groups and abutment
4. Apply order and alignment
5. Apply symmetry and matching
6. Apply spacing, stretch, and distribution
7. Apply boundary/aspect-ratio limits
8. Legalize to the grid
9. Align ports and complete routes
10. Emit shapes, devices, ports, and planned connections
```

If a constraint cannot be satisfied, generation fails with:

- the constraint type and location;
- the involved objects;
- the conflicting bounds or dimensions;
- a suggested fix when one can be determined.

No partial geometry is committed.

### 8.4 What constraints are not

- They are not a general-purpose analog placement solver.
- They do not automatically discover complex topology from a schematic.
- They do not modify geometry outside the current procedural cell or generator.
- They do not guarantee DRC-clean spacing unless the script uses the correct
  layer design rules.

## 9. Space-efficiency features

The API should make the compact choice the easy choice:

### Abutment

- Rows and columns use actual bounds and shared edges.
- `gap=0` removes gaps where geometry permits.
- Shared source/drain, shared rails, and adjacent resistor bodies can touch.

### Shared rails

- A `rail` generator spans the current row or group.
- Ports can bind to the rail.
- Nested rows can extend the same rail without duplicate geometry.

### Matching

- `mos_pair` and `cap_array` support common-centroid and interdigitated
  patterns.
- Mirroring preserves terminal orientation and label placement.

### Port-aware placement

- Ports can be aligned to cell sides or another group's port.
- Devices can be placed so that the router leaves pins toward their targets.
- `layout.connect` and `layout.route` can use port anchors without manual
  coordinate math.

### Measurement and checks

```python
layout.clearance(a, b)
layout.overlaps(a, b)
layout.contains(a, b)
layout.distance(a, b, axis="x")
layout.extents([a, b, c])
```

These helpers let generators adapt to their content rather than hard-coding
coordinates.

### Bounded compaction

- Rows and columns can redistribute slack only when explicitly requested.
- The compaction pass is deterministic and local.
- No global geometry shuffling is performed.

## 10. Existing behavior and compatibility

- Existing procedural scripts continue to run without modification.
- Existing low-level geometry calls keep their current API.
- Existing package device generator contracts remain unchanged.
- New builder features compile to the same generated-cell schema.
- Generated cells remain read-only, with the existing regeneration and fork
  behavior.
- New constraints and generators are opt-in.
- Python autodoc for `procedural_runner.py` remains the API source of truth.

## 11. Implementation phases

### Phase 1 — Handles and layout composition

- Add a common layout handle protocol.
- Add `group`, `row`, `column`, `grid`, `stack`, `repeat`, `spacer`, and
  `stretch`.
- Add bounds, anchors, port lookup, translation, mirroring, and measurement.
- Preserve the existing immediate geometry API.
- Add Python tests and autodoc.

### Phase 2 — Constraint model

- Add typed constraint records and a local constraint registry.
- Implement align, order, group, abut, symmetric, boundary, aspect ratio,
  spread, grid, and port location.
- Add deterministic resolution and diagnostics.
- Add CLI/MCP inspection of constraint metadata.
- Add golden geometry tests.

### Phase 3 — Built-in cell generators

- Add `@layout.generator` and `layout.generate`.
- Implement built-in MOS pair, resistor bank, capacitor array, via stack, rail,
  guard ring, and route-bundle generators.
- Reuse existing device PCells.
- Add generator metadata for GUI, CLI, MCP, and docs.
- Add parameter validation and regeneration hashing.

### Phase 4 — Project and package generator registries

- Add optional project `generators/` modules.
- Add optional PDK package `layout/generators/` registry.
- Version and hash generator implementations.
- Document the package generator protocol.
- Keep built-in generators as the no-package fallback.

### Phase 5 — Higher-level analog helpers

- Common-centroid and interdigitated layout helpers.
- Rail-aware routing bundles.
- Guard-ring composition.
- Matching-aware device orientation.
- Richer port and route constraints.

### Explicitly deferred

- ALIGN adapter or backend.
- General analog placement/routing solver.
- Schematic-driven automatic topology selection.
- Global compaction across unrelated groups.

## 12. Testing

Required coverage:

- handle bounds, anchors, translation, and mirroring;
- row/column/grid/stretch geometry;
- abutment and shared-rail cases;
- nested group composition;
- generator parameter validation and deterministic output;
- constraint satisfaction and failure diagnostics;
- deterministic generation hashes;
- golden geometry for a differential pair, resistor bank, and capacitor array;
- LVS/PEX smoke tests for a generated cell;
- existing procedural scripts and device generation;
- Python docstring/API documentation build;
- CLI/MCP generator discovery and constraint inspection.

A successful acceptance case should express a reference circuit noticeably more
compactly and with less coordinate math than the existing low-level script,
without changing the underlying generated-cell contract.

## 13. Risks

- **Abstraction mismatch:** a composition API can become harder to use than
  explicit coordinates. Keep low-level geometry available and prefer imperative
  rows/columns over mandatory declarative constraints.
- **Constraint ambiguity:** deterministic ordering of all constraints must be
  documented and tested.
- **Geometry ownership:** handles must not become stale after a parent moves or
  mirrors them.
- **DRC assumptions:** the builder cannot guarantee design-rule correctness;
  documentation must distinguish geometric composition from signoff.
- **Generator compatibility:** package generator APIs need versioning and a
  built-in fallback.
- **Caching:** generator code, parameters, PDK, and builder version all belong
  in the regeneration hash.
- **Hierarchy:** group fragments and generated static cells must coexist
  without duplicating geometry or breaking LVS/PEX annotations.

## 14. Relationship to ALIGN

The constraint vocabulary (`Order`, `Align`, `GroupBlocks`, `SymmetricBlocks`,
`SymmetricNets`, `GroupCaps`, `Generator`, and port classes) is intentionally
informed by ALIGN because those names are familiar and well understood.

This proposal does **not**:

- import ALIGN code or schemas;
- run the ALIGN compiler or placer;
- expose ALIGN types in OpenVLSI core;
- make ALIGN a build or runtime dependency;
- attempt to reproduce ALIGN's solver.

A future ALIGN adapter can translate the same OpenVLSI handles and constraints into
its own input format behind the existing architecture boundary. The native
procedural API remains complete and useful without it.

## 15. Open decisions

1. Should project-level generators live in `generators/` or in a dedicated
   `layout/generators/` project folder?
2. Should a generated group default to inline geometry or a reusable static
   child cell?
3. Should constraints be declared only in Python, or also exposed as editable
   metadata on `ProceduralLayout`?
4. Should the first composition API use immediate handles or deferred
   transform handles?
5. Which generator metadata is required for GUI forms in phase 1 versus phase 4?
6. Should rows and columns support fixed dimensions and slack distribution in
   phase 1, or start with content-driven sizing only?

## 16. Recommendation

Implement phases 1–3 first:

- composition handles and rows/columns;
- the local constraint evaluator;
- built-in generators for the common analog structures.

This delivers the largest usability and area-efficiency improvement without
taking on ALIGN integration or a general placement solver. Package generator
registries and an ALIGN adapter can follow once the native API has stabilized.
