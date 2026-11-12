# ICE GUI and Layout Editor

This document defines the user-interface direction for the `ice` executable. It
focuses on the desktop shell and the schematic/layout editing experience.
Domain objects and mutations belong to `icelib`; the GUI presents and invokes
them.

## Goals

The GUI should:

- provide one integrated desktop application for schematic, layout, simulation,
  analysis, and project workflows;
- make the current design state visible without becoming the source of truth;
- support both manual editing and generated-layout refinement;
- expose the same validated operations available to the CLI and Python API;
- keep long-running simulations, verification, and external-tool jobs
  responsive and observable;
- make selection, hierarchy, connectivity, and provenance understandable.

## Non-goals

The GUI should not:

- store domain state only in Qt graphics objects;
- implement geometry, connectivity, simulation, or verification logic itself;
- require a GUI process for core or CLI tests;
- silently mutate the project outside the transaction and undo system;
- hide external-tool failures behind generic success messages.

## Application shell

The main window should provide:

```text
┌──────────────────────────────────────────────────────────────┐
│ Menu / toolbar / command palette                             │
├───────────────┬──────────────────────────────┬───────────────┤
│ Project /      │ Active document views        │ Properties /  │
│ hierarchy      │ schematic, layout, plots     │ diagnostics   │
├───────────────┴──────────────────────────────┴───────────────┤
│ Selection, job progress, messages, and agent/chat panel       │
└──────────────────────────────────────────────────────────────┘
```

The shell should manage documents and views, but not own their domain data.
Panels should be dockable and replaceable without changing the core model. The
`ice` executable links against `icelib` and must not duplicate the model used by
`ice-cli`, `ice-pkg`, or `ice-mcp`.

When no project is open, the central area shows a startup view with create and
open project actions, recent projects, community links, and news. Each recent
project row has a button to remove it from the list. Document tabs and the
activity dock appear once a project is loaded, and closing the project returns to
the startup view.

## Document and project lifecycle

The GUI should support:

- creating a project;
- opening and closing projects;
- opening schematic, layout, simulation, and result documents;
- tracking unsaved changes;
- explicit save and save-as operations;
- reporting missing packages, PDKs, or incompatible schema versions;
- recovering from failed external jobs without losing in-memory state.

Creating a project asks for a project name, a project folder, and whether to
create a folder named after the project. The project root is initialized with
`project.icepro`, `sch/`, `lay/`, `impl/`, `sim/`, `scripts/`, `build/`,
`.gitignore`, and `AGENTS.md`. Schematics, layouts, implementation runs, and
simulations are stored as individual JSON files with `.icesch`, `.icelay`,
`.iceimpl`, and `.icesim` extensions.

Project state, view state, and transient job state should be separate:

| State | Examples | Persistence |
| --- | --- | --- |
| Project state | cells, nets, geometry, parameters | project files |
| View state | zoom, panes, selections, visibility | optional UI settings |
| Job state | running simulation, progress, cancellation | runtime and result records |

Schematic and layout file tabs are closable. Their labels include the file
extension (`.sp` for text SPICE schematics, `.sv` for SystemVerilog source
cells, `.icesch` for drawn schematics, `.icelay` for static layouts, or `.py`
for procedural layouts) and a leading `●` while that file has unsaved edits.
Hierarchy file entries use the same extensions; nested instance entries keep
their instance names. If
every schematic or layout file tab is closed, the central editor area shows a
form to open an existing file or create a new one. Opening a project starts with
no schematic or layout file tabs open; open files from the hierarchy or the
empty workspace form.

The GUI watches `project.icepro` and the `sch/`, `lay/`, `impl/`, and `sim/` directories.
External changes to an open file are reflected immediately when that file has no
unsaved GUI edits. If the same file has unsaved edits, its tab shows a warning
and saving asks whether to preserve the local or external version for each
conflicting file. Clean files are merged from disk even while another file has
local edits. **File → Reload Project from Disk** reloads explicitly after
confirmation.

## RTL-to-GDS phase tabs

RTL-to-GDS is part of the main document tab strip, not a separate workspace:

```text
A/MS Schematic | HDL Schematic | Synthesis | Physical Design | Layout | Simulation
```

Each top-level document tab has a custom SVG icon. Analog/mixed-signal
workspaces (`A/MS Schematic`, `Layout`, `Simulation`) use red icons; digital
workspaces (`HDL Schematic`, `Synthesis`, `Physical Design`) use blue icons.

The implementation tabs are global views over one selected implementation
run. Selecting a run in the left `Runs` tree changes the active run; selecting
one of its physical stage rows switches the `Physical Design` pane. Multiple
runs therefore do not multiply the top-level tab count. Execution remains
available through `ice-cli implementation-run`, MCP, and `icepy`, and its output
is reported in the bottom Messages dock.

`Physical Design` is one workspace with a stage bar and a left `Runs` tree:

```text
Floorplan → Placement → CTS → Routing → Final
```

The stage bar shows a status marker for each checkpoint. Each stage pane uses
the same three-column shell as Schematic and Layout:

- **Left:** a compact run row with the run selector and icon actions, followed
  by the `Runs` tree, a `Display` tree, and an `Artifacts` list. In the physical
  workspace the tree shows the physical stages (Floorplan, Placement, CTS,
  Routing, Final) for each run. Selecting a stage row switches the stage pane.
  Floorplan also has a `Netlists` explorer listing technology-mapped `.v`
  files. The icon actions are **Run stage**, **Run from here**, **Run all**, and
  **Cancel**; a small contextual hint below the row shows the next
  prerequisite, the ready action, or the last failure. **Open in OpenROAD GUI**
  launches the standalone viewer on the active ODB, and Final adds **Import
  final layout**. The `Display` tree toggles layers, net types, instance
  categories, special nets, ports, route guides, rows, and blockages. Fill,
  endcap, tap, antenna, and cover cells are hidden by default; changing
  visibility preserves zoom and pan.
- **Middle:** an interactive canvas. The physical stages load their stage ODB
  through the `ice-openroad` reader asynchronously, with the legacy DEF import
  as a fallback. The toolbar provides fit, zoom, find, rulers, labels, clear,
  and image export. Clicking an instance, port, routed shape, or DRC marker
  selects it and fills the Inspector. Right-clicking can focus a net, zoom to
  the selection, or clear it. The status line shows cursor coordinates in
  microns. The **Heat map** selector shows embedded placement-density or
  routing-density maps; **Load CSV** displays any exported OpenROAD heat map,
  and **Export CSV** writes placement, routing, power, pin, RUDY, or IR-drop
  maps. Heat-map settings provide log/reverse scale, automatic or explicit
  range, alpha, and a legend. When the ODB reader truncates the preview, the
  canvas shows a warning. Double-clicking an artifact opens it in a closable
  tab; large files and ODB databases open externally.
- **Right:** the stage settings form first, followed by `Inspector`, `Metrics`,
  `DRC`, `Timing`, `Clock`, `Hierarchy`, and `Properties`. The Inspector shows
  the selected object's name, master, category, module, net, layer, and
  sources. DRC lists TritonRoute violations and double-clicking one selects and
  zooms to the marker. Timing groups run metrics and can generate an OpenROAD
  STA path report. Clock lists clock nets, and Hierarchy shows the module tree.

The HDL Schematic page shows the project's `.sv` files in the left `Explorer`,
with a search field and icon buttons to create (`+`) or delete the selected file.
Selecting a file opens or activates it in a closable middle tab with its own
SystemVerilog editor. The right column has `Outline` and `IP/Libraries` tabs.
`Outline` lists modules, ports, parameters, always blocks, assignments, and
functions/tasks with click-to-line navigation. `IP/Libraries` lists project
packages, installed PDK packages, and reusable HDL modules. **New HDL Block**
creates a new `.sv` entry with a starter module template. SystemVerilog block
creation lives there, not in the A/MS Schematic tab.

The Synthesis page uses an HDL file Explorer with an **Elaborate design** action
next to its search bar. Below that Explorer, a `Runs` tab contains a search bar,
a `Run`/`Stage` table, and **Synthesize**, **Show graph**, **Export .v**, and
**Remove run** actions. **Elaborate design** prompts for a PDK, creates a run, and runs Yosys
elaboration; the table shows `elaborating` and then `elaborated`, and the run
gets an `elaboration.svg` artifact. **Synthesize** prompts for a PDK and
produces `synthesis.svg` plus the generic synthesized netlist `1_synth.v`.
**Export .v** runs the Yosys technology-mapping step against the platform
Liberty files, writes `synth/2_tech_map.v` directly into the run directory, and
adds it to the synthesis artifact list. **Show graph** opens the elaboration
and/or synthesis graph as document tabs. **Remove run** deletes the selected
run from the project and removes its `build/impl` directory. Graph tabs render
SVG and recolor Graphviz output to match the active dark or light theme.

The Synthesis page uses the same padded Explorer and artifact containers. Its
left column lists HDL source files. The middle panel has no permanent graph
tabs: double-clicking an artifact opens one closable tab per file, just like a
document. Graph artifacts open a zoomable SVG canvas and the technology-mapped
netlist opens a read-only Verilog editor. The right column lists the
`elaboration` and `synthesis` graph artifacts and the technology-mapped
`2_tech_map.v` netlist. Logs, scripts, and the generic `1_synth.v` stay in the
run directory but are not shown in this list.

Graph generation is split from the synthesis itself so a large design cannot
block the stage. Yosys writes DOT files, then ICE renders them to SVG with a
bounded Graphviz run. When the detailed post-synthesis graph is too large to
lay out, ICE uses a compact graph captured before memory and technology
mapping, so the result stays viewable. If no graph can be rendered, the DOT is
kept as a text artifact and a warning is written to the Jobs/Messages views.

The Floorplan stage pane adds a `Netlists` tab to its left column. It lists
every `synth/2_tech_map.v` and exported technology-mapped `.v` present across
runs; activating an entry selects that run. Floorplan reads the run's
`2_tech_map.v`, so exporting from Synthesis is required before running the
floorplan stage. Its settings expose utilization (a fraction of the core area,
so `0.5` means 50%), aspect ratio, optional explicit die origin/width/height,
optional explicit core origin/width/height, placement site, IO
horizontal/vertical layers, threads, and timeout. Applying settings updates the
run configuration and invalidates the floorplan, placement, CTS, routing, and
final checkpoints so stale results are never presented as current.

The Placement, CTS, Routing, and Final stage panes use the same pattern:

- prerequisites are enforced from stage status:
  - placement requires a successful floorplan;
  - CTS requires placement;
  - routing requires CTS, or placement when CTS is disabled;
  - final requires routing;
- editable settings:
  - placement: density, timing-driven, routability-driven;
  - CTS: enable/disable CTS, root buffer, clock buffer list;
  - routing: minimum/maximum routing layers, antenna repair;
  - final: RC extraction follows the platform rules;
  - every stage also exposes threads and timeout;
- applying settings invalidates that stage and every later checkpoint;
- the middle canvas refreshes from `3_place.odb`, `4_cts.odb`, `5_route.odb`,
  or `6_final.odb` after the stage completes, and shows pending/failed/skipped
  messages instead of stale geometry;
- the **Heat map** selector can color the preview by placement density or
  routed wire density, and **Export CSV** writes placement, routing, power,
  pin, RUDY, or IR-drop heat maps under the run's `reports/` directory.

The Inspector, Metrics, DRC, Timing, Clock, Hierarchy, and Properties tabs
continue to show the active run, selected stage, and selected preview object.

Graph generation requires Graphviz (`dot`) on `PATH`; without it, synthesis
still runs and the canvases show an explanatory message. Importing a finished
DEF into a layout cell is exposed through `ice-cli implementation-import`, MCP,
and `icepy`.

## Command and transaction model

User actions should be expressed as core commands or application services.
The GUI should:

1. collect interaction-specific input;
2. validate or preview the requested operation;
3. submit a core mutation;
4. display diagnostics and affected objects;
5. update views from model notifications.

Every mutation that changes project state should be undoable where practical.
Multi-step operations such as routing, import, generation, and agent actions
should be grouped into one transaction or operation batch.

## Selection and cross-probing

Selection should use stable domain IDs rather than pointers to graphics items.
Selecting an object should be able to reveal related objects across views:

```text
Schematic instance
    ↕
Layout instance / extracted device
    ↕
Net and simulation data
```

The GUI should distinguish:

- primary selection;
- related/highlighted objects;
- hover/preselection;
- locked or read-only objects;
- objects affected by a diagnostic.

Cross-probing should remain available even when one representation is absent or
out of date, with the relationship status shown explicitly.

## Schematic editor

The schematic editor should initially provide:

- an infinite panning canvas with origin axes;
- pan, zoom, grid, snapping, and fit-to-view;
- symbol placement and movement;
- wire and net-label creation, with double-click rename for net labels;
- pin and net inspection;
- instance/property editing;
- hierarchy navigation;
- selection and cross-probing;
- validation diagnostics;
- undo/redo.

The view renders schematic database objects. Symbols remain semantic objects
with artwork and pin metadata; Qt graphics items are projections of that data.
Scroll the mouse wheel to zoom around the cursor.
Connecting two pins merges their nets, and schematic wire routes are cached per
net and branch. Editing a connection re-routes only the affected net, while
moving or rotating a symbol re-routes nets attached to it or whose wires
intersect its new geometry. Unrelated nets keep their existing routing.
Dragging a net label stores a route-relative anchor: the branch endpoints, the
normalized distance along that branch, and the label side (A or B). Labels
therefore follow translated or re-routed wires, survive save/load, and do not
depend on volatile segment numbering. Source reference labels sit to the left
of their symbols. Passive values are shown below resistors, capacitors, and
inductors. Source values are shown to the right, with sine sources displaying
amplitude and period. Selecting a symbol highlights its artwork, reference
label, and value text without drawing a selection rectangle. Hovering over a
symbol highlights its artwork, and hovering over a net highlights its wires.
The **+** button in Schematic **Hierarchy** offers **Schematic**, **Text SPICE
Schematic**, and **SystemVerilog Block**. Text SPICE schematics use a
notebook-text icon, appear in the schematic tab bar with a `.sp` extension, and
open a plain-text SPICE editor in place of the canvas and canvas toolbar.
SystemVerilog cells use a notebook-text icon, appear with a `.sv` extension, and
open the same source editor with SystemVerilog templates and syntax
highlighting.
Their source is stored directly in the project and saved with it. Source cells
are not projected into generated hierarchy symbols, but they can be selected as
the source for a simulation. For a text schematic, simulation uses the stored
SPICE source minus analysis-control directives such as `.tran`, `.dc`, `.ac`,
`.op`, and `.end`. The **Build Netlist** button and the schematic signal picker
are hidden for source cells; only **Run Simulation** is shown, and it runs
directly without a separate build step.

The Simulation panel **+** menu offers **Add Simulation…**, **Add SystemVerilog
Testbench…**, and **Add Simulation Script…**. **Add SystemVerilog Testbench…**
creates the testbench source cell and a simulation that targets it with the
**Icarus Verilog (SystemVerilog)** simulator and **Digital / VCD** analysis, then
opens the **Testbench** tab in the simulation editor. The tab edits the source
directly with SystemVerilog syntax highlighting and has its own **Run
Simulation** button. Selecting a SystemVerilog
cell created elsewhere in the Simulation tab switches the simulator and analysis
the same way. The source is compiled with `iverilog`, run with `vvp`, and the
generated VCD is loaded as the result traces. If the source has no
`$dumpfile`/`$dumpvars`, ICE adds a small wrapper module that dumps the design.
The first successful run fills the **Signals to capture** list and any empty
graph Y selections from the VCD traces.

A drawn schematic with SystemVerilog instances is detected as mixed-signal.
The simulator selector then defaults to **Mixed-signal (ngspice + Icarus)** and
the analysis to **Transient**. The resulting graph contains both the analog
node traces and the digital `<instance>.<port>` traces. HDL instances are placed
by MCP/CLI with a generated symbol whose pins match the module ports; the same
symbols render and wire normally in the schematic canvas.

The graph toolbar has a **Load VCD** action that imports an external VCD file
into the current simulation. Its traces replace the capture list, populate
empty graphs, and are shown immediately without running a simulator.

Future schematic features include buses, reusable libraries, ERC, netlisting,
and richer hierarchy editing.

## Layout editor

The layout editor should initially provide:

- layer-aware rendering;
- an infinite panning canvas with origin axes;
- grid, snapping, and fit-to-view;
- rectangle and polygon creation;
- shape selection and movement;
- ports, labels, and net visibility;
- hierarchy navigation;
- instance transforms;
- basic Boolean operations;
- spatial queries for selection and editing;
- diagnostic markers;
- undo/redo.

The Layout left column contains **Hierarchy** and **Devices** tabs; the right
column contains **Properties** and **Layers**. The layout creation dialog asks
for the layout PDK and, when that package declares stack variants, the stack
(for example GF180MCU `3LM_1TM` through `6LM_1TM`). The package's default stack
is preselected. The selected PDK and stack are stored on the layout cell and
shown in its properties; a layout instance must use the same PDK and stack as
its parent. Creating a layout with a packaged PDK adds that package's complete
device-library layer set for the selected stack up front; placing devices
reuses those layers instead of adding new ones. The **Layers** panel
groups GDS purpose variants under their base layer number: for example
`Metal1.pin` and `Metal1.nofill` appear under `Metal1`. Pin, mask,
fill-blocking, and marker layers that use non-zero datatypes or marker base
names start hidden; enable them with the child checkbox or the parent group
checkbox. PDK packages may supply default layer colors, patterns, and
visibility in `layout/layer-styles.json`; these styles are applied when the
package's layers are added, and layers without an entry keep the built-in
number-based palette. Creating a custom layer opens a form with the layer name, assigned
GDS number, color picker, fill pattern, and a live style preview. The toolbar
next to the search field can show only drawing layers or only pin layers. Its
single eye button shows all layers when any layer is hidden and switches to
hide all when every layer is visible. Layout entries use a notebook-pen icon.
Each layout node lists its child layout/device instances, planned devices,
rectangles, polygons, paths, and texts directly, with a distinct icon for each
entry type. Right-click an entry and choose
**Rename…** to rename a layout, script, instance, planned device, or shape. Right-click a layout entry
and toggle **Flatten Layout for DRC** to control whether **Run DRC** exports the
flattened instance geometry instead of only the layout cell's own shapes.
Right-clicking a schematic in the Schematic **Hierarchy** tab offers **Make
Layout from Schematic**. This creates a layout with the schematic's technology
and projects every schematic instance into a movable **Planned Device** box
under the new layout. The boxes keep the relative positions the symbols had in
the schematic instead of being arranged in a grid. Nets are drawn as straight
connections between the boxes;
planned devices use a box icon in the hierarchy and can be selected and moved
directly on the canvas. Use **Hierarchy Go Up** and **Hierarchy Go Down** in the
left layout tool column to open the parent layout or the selected child
instance.

With nothing selected in the layout, **Properties** shows a fitted preview of
the layout geometry and a **Grid** section with the Layout-only snap spacing in nanometers.
The layout toolbar keeps the grid visibility and snap toggles. The layout cell
parameter table is hidden while nothing is selected. Selecting a rectangle
shows its name, type, layer, and editable width and height in database units;
selected polygons, paths, and texts show read-only name, type, and layer
information.

The **+** button in the Layout **Hierarchy** offers **Static Layout** and
**Procedural Layout**. Procedural layouts use a notebook-text icon, open a
Python script editor with syntax highlighting in the middle view, and contain a
generated static layout as their child. The canvas tool columns are hidden while a procedural layout
tab is active. The script runs with a `layout` builder that exposes
`layout.layer()`, rectangles, polygons, ports, and instances. Right-click a
procedural layout and choose **Regenerate layout** (or use the editor's
**Regenerate layout** button) to replace the generated child's geometry from
the current script. Scripts run with `python3`, or the interpreter selected by
`ICE_PYTHON`. A failed run reports the Python error and leaves the previous
generated geometry unchanged. The layout API also exposes
`layout.add_dummy_fill(...)`. This is intentionally PDK-agnostic: ICE forwards
the request to the active package's `layout/dummy_fill.py` implementation after
the user script finishes. The IHP SG13G2 package implements square dummy metal
fill for Metal1-5 and TopMetal1-2 using rules adapted from the `gdsfill`
project. A PDK without that implementation reports a clear error when the API
is used. The active PDK package's layout device library (`layout/devices.json`)
appears in **Devices**, with search, category filtering, and a geometry preview
in **Properties**. IHP SG13G2, Sky130, and GF180MCU packages ship their own
libraries, and switching the active layout to a cell from another PDK loads that
package's devices. The device list starts with no selection; selecting a device
switches to **Place Device** and shows a cursor-following preview, while
activating **Select** clears the list selection.
Clicking in the canvas imports the device geometry into the project and creates
a layout instance. Every placed device gets its own read-only library cell, so
repeated placements do not share parameters or geometry. Each instance appears
separately under its parent layout in the hierarchy and can be selected, moved,
and deleted directly in the canvas. Selecting a placed instance in the
hierarchy highlights it in the canvas, and selecting an instance in the canvas
highlights its hierarchy row. PCell parameter names, values, and
descriptions are shown in **Properties** for the device preview and for imported
library cells. Selecting
a placed device instance shows that instance's own geometry preview and
parameter values. Editing a parameter regenerates that instance's geometry
through the package-local generator without changing other instances. Editing
a device in the **Devices** tab changes the defaults used by future placements.

The generator contract is
`<pdk-root>/layout/device_generator.py --pdk-root <package> --module <module>
--class <class> --params-file <json> --output <json>`. IHP evaluates the
vendored upstream PyCell sources (and KLayout's `pya` module) through
`icepy.pcells`. Sky130 and GF180MCU use ICE-native PCells implemented directly
with the `icepy` layout builder. ICE uses `python3` unless `ICE_PYTHON` points
to another interpreter. If the package generator or Python module is
unavailable, ICE reports the error and leaves the existing geometry unchanged.

Device placement ghosts and the Properties layout preview use the same layer
color resolution and flattened geometry renderer as the canvas, including
paths, text, pins, and planned devices. Layer colors are keyed by GDS layer
number, so they remain stable when layers are added or removed.

The layout toolbar separates **Select** from **Move**. **Select** supports
rubber-band selection and handle editing: drag path points, polygon vertices,
and rectangle corner or edge handles, but it does not move whole objects.
**Move** drags rectangles, polygons, paths, texts, planned devices, and placed
instances; selection handles are hidden while Move is active.

In **Select**, click an already-selected rectangle or polygon again to trace and
highlight its net in the layout. Tracing follows touching conductive shapes and
crosses layers through via shapes, using a connectivity stack inferred from the
project's `Metal*`, `TopMetal*`, `Via*`, `TopVia*`, and `Contact` layer names.
For example, `Metal1` connects to `Metal2` only through `Via1`. `Contact` links
`Activ` and `GatPoly` (or the older `Poly2` name) to `Metal1` when those layers
exist. Shapes on layers that do not belong to the stack are reported in the
status bar instead of being traced.

Net labels are taken from shape `net` assignments or text shapes placed on the
traced geometry; otherwise the trace is shown as **Traced net**. Related planned
connections and planned devices are included when they touch the traced
geometry. The canvas and status bar show the net name and a summary with shape,
via, and layer counts. **Tools → Net Trace Layer Stack** shows the inferred
stack and via connections. The highlight clears on the next click, selection
change, or tool change.

The **Create Polygon** tool draws a filled polygon on the selected layer. Click
each vertex; Shift constrains the current edge to horizontal or vertical.
Double-click, Escape, or Space closes the polygon and leaves the tool. Select a
polygon to show square vertex handles; drag a handle to move that vertex.

The layout toolbar also provides a **Draw Path** tool for routing. Click to set
the path start and click again to commit each segment. Escape or Space finishes
the current path and leaves the tool. Holding Shift
constrains the active segment to horizontal or vertical so all turns are right
angles. While the Path tool is active, **Properties** shows the current path
layer and an editable `width (nm)` value (200 nm by default). After the first
path click, scroll the wheel to move the next segment up or down one metal
layer; ICE inserts the corresponding via at the connection point automatically.
For the IHP stack, for example, scrolling from Metal2 down to Metal1 adds a
Via1 at the junction. When no path is being drawn, the mouse wheel zooms the
Layout canvas; Ctrl+wheel also zooms while a path is active. A routed path is
stored as one path shape
with an editable centerline, width, and cached outline; it moves as a single
object rather than as independent segment rectangles. The Draw Path tool and
manually drawn paths use the waypoints icon, while automatically routed paths
are listed as Routed Path shapes with the route icon. Select a manual path to
show its point handles; drag a handle to move a point, double-click a segment to
insert a point, and right-click a handle to delete it. Automatically routed
paths expose handles only at their endpoints; drag an endpoint to a different
border of its pin to change which side the route attaches to. Routing avoids
other automatically routed paths on different nets by choosing another metal
layer, trying an alternate Manhattan trace, or bridging a segment through an
adjacent layer with vias. If no conflict-free route exists, the direct route is
kept. Changing layers while drawing
keeps the same path and records the layer for each segment; changing the width
starts a new path at the current endpoint. Corners and endpoints are square,
not rounded. Layer-change vias are stored with the path and move with it.
Segments adjacent to a via extend by half the via size so the copper fully
overlaps the via on both layers.

The **Create Rectangle** tool is click-move-click: the first click anchors the
corner, moving the mouse previews the rectangle and shows its live `width x
height`, and the next click finishes it. The size field at the bottom right of
the canvas accepts exact dimensions while the rectangle is previewed; type
`width,height` (for example `281,2875`) and the rectangle is created with those
dimensions once typing pauses, or press Enter to finish immediately. Dimensions
follow the layout **Spacing** setting when snap-to-grid is enabled. Escape
cancels the pending rectangle.

Select a rectangle to show square point handles at its four corners and at the
midpoints of its left, right, top, and bottom edges. Drag a handle to resize
from that corner or side while the opposite side stays fixed. The handles use
the same square style as path points, and dragging the rectangle body moves it.

With two or more rectangles or polygons selected on the same layer, the
Boolean buttons in the right layout tool column perform **Union**,
**Intersection**, **Difference**, and **Exclusive Or**. The operands are
replaced by one or more polygon shapes named after the operation. **Difference**
subtracts the newer shapes from the oldest selected shape. Results with holes
are split into independently fillable polygons, and an operation that produces
no geometry leaves its operands unchanged. Use undo to restore the original
shapes.

The **Ruler** tool measures between two clicked points. The ruler line keeps a
constant screen width as you zoom, and shows minor and major tick marks with
distance labels along its length.

The **Comment** tool (Shift+C, layout text labels) places an annotation on the
currently selected layer. Click the canvas, enter the string, and ICE creates a
movable text object with a 1 um nominal height. It is stored in the project and exported as
GDSII TEXT records, but it does not create conductive geometry.

The **Draw Text Polygon** tool is available from the dropdown attached to the
**Create Polygon** toolbar button. It converts the glyph outlines into actual
filled polygon shapes on the selected layer. Glyph contours are simplified to
45- and 90-degree edges, and each connected glyph component becomes one polygon
with counters represented by a keyhole cut. Letters with disconnected parts,
such as `i` and `j`, use one polygon per component. The click position becomes
the lower-left corner of the glyph bounds. All components appear as one
**Text Polygon** entry in the Hierarchy and select, move, rename, and delete as
a single object, while still participating in connectivity, DRC, and GDSII
BOUNDARY export like ordinary layout polygons. Text polygons are excluded from
Boolean operations.

The **Create Pin** tool places a named layout pin on the currently selected
layer. Drag from one corner to the other to define the pin rectangle; the
preview uses the selected layer style while dragging. Pins are visible as
layer-colored rectangles with centered labels, are selectable/movable/resizable,
and can be locked or hidden. The Properties panel
edits the pin name, label, direction, physical side, net, size, visibility, and
lock state. Pins are exported as GDSII boundary geometry plus TEXT labels.
Layout nets are created through the pin net field or
`ice-cli layout-add-net`; assign a net by name in Properties or with
`ice-cli layout-assign-port-net`. The **Hierarchy** panel lists each layout's
pins with visibility checkboxes and pin details in the tooltip. Child-cell pins
are drawn read-only inside instances; double-clicking one promotes it to a
parent pin that keeps the source instance and child port link.

**File → Open Layout Interchange** imports GDSII and asks which layer carries
pin-label TEXT records; use `-1` to import text as ordinary text shapes only.
**Save Layout Interchange** writes the active cell as GDSII. LEF and DEF pin
exchange is available through `ice-cli` and `ice-mcp`.

Select a planned device box to highlight it. Double-click it, or right-click it,
to choose **Draw Layout Manually** or **Place Layout Device…**. Voltage/current
sources, ground symbols, and `Input`/`Output` port symbols instead offer
**Convert to Pin**: a dialog lets you choose the pin
layer, then the block is replaced by a single layout pin (an existing pin on the
connection's net is reused when there is only one net), and all planned
connections from the block are retargeted to that pin. Converted `Input` and
`Output` pins keep the corresponding port direction and name. The placement
dialog lists the active PDK's PCells, suggests
devices by model and pin compatibility, and lets you edit the PCell parameters
before placement. Placing the device replaces the planned box with a layout
instance and rewires the existing planned connections from the box to the new
PCell pins. After placement, select the instance and edit its PCell parameters
in **Properties**. Select a planned connection to highlight it. Double-click it,
or right-click it, to choose **Draw Connection Manually** or **Route
Automatically**. Automatic routing is enabled when both
ends are placed device pins or layout pins; it routes on metal drawing layers,
transitions from non-metal pins through the required contact/via stack, and
leaves/enters rectangular pins from the middle of their narrowest sides. Automatically routed
paths are recomputed when either endpoint moves, rotates, snaps, or is
regenerated from changed PCell parameters, so the route stays attached to the
device pins. The route leaves each pin outwards, turns with Manhattan segments,
and can be edited with the normal path tools; moving either endpoint recomputes
it and replaces those manual edits. Undo the routing to restore the planned
connection.
Manual connection removes the logical planned connection and switches to the
path tool.

**Tools → Run DRC** exports the active layout cell to a temporary GDSII file,
runs `gdscheck` with the process deck derived from the cell's PDK, and parses
the generated `.lyrdb` report. Violations appear in the **DRC Report** tab of
the activity dock with severity, rule, message, cell, and location columns.
Clicking a violation row opens the checked layout and centers the canvas on
the reported location with a temporary highlight.

**Tools → Run LVS** compares the active layout against its source schematic.
ICE infers the schematic from planned-device links, then from matching layout
and schematic names, and asks for a schematic when the match is ambiguous. LVS
checks device presence, reference names, device types, shared parameters on
placed device instances, and net topology from planned connections. Device
parameters are compared numerically when both sides use compatible quantity
values, so `0.15u` and `150n` are equivalent. If the layout has no planned
connectivity, LVS reports a warning and compares devices only. Primitive
sources and ground symbols are treated as external references rather than
layout devices, and pins converted from them are not compared as schematic
ports. Top-level schematic ports created with `Input`/`Output` symbols are
matched to layout pins by name, including pins produced by **Convert to Pin**;
missing pins, extra pins, direction changes, and net-binding changes are
reported. Results appear in the **LVS Report** tab with mismatch categories
and schematic/layout object ids. Text SPICE and SystemVerilog source cells are
not supported by LVS yet.

**Tools → Run PEX** (or right-clicking a layout cell and choosing
**Extract Parasitics (PEX)**) extracts first-order parasitics from the active
user layout cell. It groups geometry into nets with the same connectivity used by the
layout connectivity view, recognizes `Metal*`, `GatPoly`/`Poly*`,
`Activ`/diffusion, and via/contact layers, and reports each net's area,
perimeter, total resistance, and capacitance in the generated SPICE text
schematic. A text SPICE cell named `<layout> PEX` is created — or updated on a
later run — and selected in the Schematic tab, so it appears in the project
hierarchy. Single-terminal nets use a series resistance with the capacitance to
ground; multi-terminal nets use a resistance star between pins. Placed
device-library instances are extracted first: their terminal pin rectangles are
transformed into parent coordinates, matched to the parent nets they overlap,
and emitted as SPICE primitives or PDK subcircuits with model parameters. If no
placed devices are recognized, PEX falls back to copying the inferred source
schematic's device netlist. Devices drawn purely as polygons are recognized
through the PDK package's `layout/extraction.json` rules: PEX cuts active under
gates, finds source/drain terminal nets, and emits MOS devices with geometric
W/L. The same rules can describe simple resistors and capacitors. Use LVS to
verify the result before relying on it. The generated deck records the
coefficients used, and the API/MCP surfaces can override them for a
process-specific first pass.

The editor must support integer database coordinates and a clear display-unit
conversion. It should never use display precision as a substitute for database
precision.

Generated layout should enter the same editor and database as manually created
layout. The user should be able to inspect generated provenance, edit geometry,
and continue with Python or agent operations without exporting to an
intermediate format.

## Keyboard shortcuts

Editor tool shortcuts are active while the schematic or layout canvas has
focus. The same key may be used for different tools in the two editors.

Common defaults:

- **Select**: Space
- **Place device**: P
- **Delete selection**: Del
- **Run simulation / DRC / LVS**: F6 / F7 / F8
- **Fit to view**: F
- **Toggle grid**: Ctrl+G
- **Snap to grid**: Shift+G
- **Hierarchy up / down**: Alt+Up / Alt+Down
- **Schematic rotate / flip**: Shift+R / H
- **Schematic place device**: R resistor, X NMOS, I input, O output, C capacitor, D diode, G ground, L inductor, S sine voltage source; Shift+wheel scrolls the device library while placing
- **Schematic comment**: Shift+C
- **Layout rotate / flips**: Shift+R / H / J
- **Layout rectangle / polygon / path / comment (text label) / text polygon / pin / ruler**: R / G / W / Shift+C / Shift+T / N / L
- **Layout boolean union / intersection / difference / XOR**: U / I / D / X

Use **Settings → Keyboard Shortcuts…** (Ctrl+Shift+K) to reassign commands,
clear a binding, reset one command, or reset all commands. Global commands work
anywhere in the window; schematic and layout entries are scoped to their editor
canvas so the same key can be reused. Conflicts within a scope, or against a
global shortcut, are rejected. Bindings are stored in the user's Qt settings and
applied immediately.

## Jobs dock

The **Jobs** tab lists long-running implementation work: elaboration,
synthesis, technology mapping, and physical stages. Each row shows the job,
stage, status, elapsed time, and the latest log line. The footer has an
indeterminate progress indicator while work is running, **Cancel selected job**
for the active implementation process, and **Clear finished** to remove
completed entries.

Elaborate and Synthesize now return immediately: the project setup is deferred
to the next event-loop turn, the CLI process runs asynchronously, and the new
job appears in the Jobs tab before the run starts. Selecting a job enables
cancellation, and stage transitions update the job in place. Detailed output is
also available in the category-filtered **Messages** tab.

## Messages dock

The bottom **Messages** tab is the unified log view. Each log category has its
own spdlog sink and logger:

- **Application**
- **Simulation**
- **Implementation**
- **DRC**
- **LVS**

Messages are shown with time, severity, category, and text. The category and
severity selectors filter the view without changing the stored messages; the
severity selector shows that level and above. Severity colors distinguish
warnings and errors. Simulation status, warnings, errors, and stored-run paths
are written here instead of appearing below the netlist pane. Implementation
stage output, DRC summaries/violations, LVS summaries/mismatches, and generic
application messages use the same category-aware sinks.

## Simulation scripts

The Simulation panel **+** menu offers **Add Simulation…**, **Add SystemVerilog
Testbench…**, and **Add Simulation Script…**. Simulation scripts are
project-level Python files under `scripts/`,
recorded in `project.icepro`, and listed as top-level entries in the simulation
hierarchy alongside simulations. Adding a script does not require an active
simulation and always creates a new Python file from the built-in template.

Selecting a script shows the project-level script view in the Simulation
workspace, replacing the simulation editor on the right. It is not a separate
top-level document tab and is not stored inside any simulation. The right pane
is an editable Python source editor with syntax highlighting. Script run
messages, errors, and Markdown output are appended to the **Messages** tab,
which is opened when a script runs. Scripts have access to every stored
simulation run through the context (`simulations`, `runs`, `latest`,
`latest_many`, and `load`) and can compare multiple simulations in one script.

Scripts can generate GUI graphs with `ctx.graph(...)`. The generated graphs are
shown in tabs below the script editor. Graphs can reference stored runs and
signals, or use explicit `x` and `y` arrays computed by the script. Running the
script replaces the previously generated graph tabs. The latest graph payload is
cached under `build/simulation-scripts/` and reloaded when the script is selected,
including runs started from `ice-cli`.

The **Save** button saves the Python source immediately; edits also autosave
after a short pause. **Run** saves any pending edits and executes the script.
**Open File** opens the Python source externally. Deleting a script removes only
its project metadata; the file remains on disk.

Simulation plots downsample traces to at most two display points per horizontal
pixel column. The minimum and maximum sample in each column are kept so narrow
spikes remain visible, while the painter path stays proportional to the plot
width instead of the stored sample count. Rendered paths are cached per plot
size, graph definition, and result, so switching graph tabs does not re-render
large traces. This keeps large transient results responsive without changing the
stored simulation data.

## Agent tab

The **Agent** tab is the Agent Client Protocol (ACP) chat interface. It uses
`agentsdk-cpp` (`acp_client`, `acp_session`, and `acp_agent_manager`) to launch a
local ACP-capable agent as a subprocess over newline-delimited JSON-RPC on
stdio.

- The compact right panel has two pages. The first selects an ACP agent and
  connects it. **Connect** launches the agent and creates the first ACP session,
  but stays on the connection page; **Session** opens the session page.
- The session page has a session selector plus **+** and **x** controls to
  create and close sessions. All sessions share one agent process and ACP
  connection. Each session keeps its own transcript, model, and token usage,
  and sessions can be switched while a turn is running.
- If the agent supports ACP session history, earlier sessions for the current
  project appear below the open sessions (marked **↺**). Selecting one loads it
  with `session/load` and replays its conversation into the transcript.
- The session page also shows the **Model** selector and MCP server list.
  **Agent** returns to the connection page; **Disconnect** ends the agent and
  all of its sessions.
- The **Model** selector is populated from the active session's ACP config
  options and changes that session's model through `session/set_config_option`.
- The prompt row sends a user turn. The transcript shows assistant text,
  thoughts, and plans. Tool-call updates are coalesced by ACP tool-call ID:
  completed file reads are grouped as `→Explored: N reads`, shell commands are
  shown as `$ command` with truncated output, and pending/in-progress updates do
  not create duplicate transcript entries. **Cancel** sends an ACP cancellation
  for the active session's prompt.
- Agent permission requests open a modal dialog listing the options supplied by
  the agent.
- Agent questions use ACP form elicitation. ICE advertises form support, renders
  the requested fields (single choice, multi-select, boolean, numeric, and text),
  and returns the answers to the agent. Declining the dialog returns a declined
  elicitation response.
- Token usage updates the session page when the agent reports `usage_update`.

ACP support is built from the bundled `third_party/agentsdk-cpp` submodule into
a shared `libagentsdk` library. Initialize the submodule with
`git submodule update --init --recursive` before building. The installed
`ice`/`ice-mcp` binaries load the shared library from the installation's `lib`
directory.

## Styling

The application uses Qt's **Fusion** style. By default it follows the system
appearance (light or dark) through the platform color scheme. **Settings →
Appearance** allows overriding this with **System Theme**, **Light Theme**, or
**Dark Theme**; the choice is persisted with `QSettings`. Custom-painted views
such as the schematic, layout, and simulation plot derive their colors from the
active QPalette so they stay consistent with the surrounding widgets.

## Interaction conventions

The GUI should establish consistent behavior for:

- selection and multi-selection;
- snapping and grid overrides;
- copy, paste, move, rotate, mirror, and delete;
- constraint or validation failures;
- read-only and locked objects;
- modal versus non-modal operations;
- keyboard shortcuts and command palette actions.

Commands that can be invoked from the command palette should map to the same
application operations used by CLI and automation surfaces.

## Long-running jobs

Simulation, DRC, LVS, PEX, import/export, package operations, and layout
generation should run through a job abstraction that exposes:

- queued, running, succeeded, failed, and cancelled states;
- progress where available;
- logs and structured diagnostics;
- cancellation;
- links to generated artifacts and affected project objects;
- reproducibility/provenance metadata.

The GUI should remain usable while jobs run. A failed job must not be rendered
as a successful project mutation.

## Diagnostics

Diagnostics should have:

- severity;
- stable code;
- human-readable message;
- source object or file;
- geometry location when applicable;
- suggested action when available;
- originating subsystem or external tool.

Diagnostics should be inspectable in a panel and navigable from the relevant
view. External tool output should be retained rather than reduced to a single
status string.

## Accessibility and usability

The GUI should plan for:

- keyboard access to core operations;
- visible focus states;
- scalable text and UI density;
- color-independent diagnostic indicators;
- readable layer and net identification;
- non-modal alternatives for long-running work;
- predictable undo/redo behavior.
