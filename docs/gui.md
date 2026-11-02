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
`project.icepro`, `sch/`, `lay/`, `sim/`, `scripts/`, `build/`, `.gitignore`,
and `AGENTS.md`. Schematics, layouts, and simulations are stored as individual
JSON files with `.icesch`, `.icelay`, and `.icesim` extensions.

Project state, view state, and transient job state should be separate:

| State | Examples | Persistence |
| --- | --- | --- |
| Project state | cells, nets, geometry, parameters | project files |
| View state | zoom, panes, selections, visibility | optional UI settings |
| Job state | running simulation, progress, cancellation | runtime and result records |

Schematic and layout file tabs are closable. Their labels include the file
extension (`.sp` for text SPICE schematics, `.icesch` for drawn schematics, or
`.icelay` for layouts) and a leading `●` while that file has unsaved edits. If
every schematic or layout file tab is closed, the central editor area shows a
form to open an existing file or create a new one. Opening a project starts with
no schematic or layout file tabs open; open files from the hierarchy or the
empty workspace form.

The GUI watches `project.icepro` and the `sch/`, `lay/`, and `sim/` directories.
External changes to an open file are reflected immediately when that file has no
unsaved GUI edits. If the same file has unsaved edits, its tab shows a warning
and saving asks whether to preserve the local or external version for each
conflicting file. Clean files are merged from disk even while another file has
local edits. **File → Reload Project from Disk** reloads explicitly after
confirmation.

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
- wire and net-label creation;
- pin and net inspection;
- instance/property editing;
- hierarchy navigation;
- selection and cross-probing;
- validation diagnostics;
- undo/redo.

The view renders schematic database objects. Symbols remain semantic objects
with artwork and pin metadata; Qt graphics items are projections of that data.
The **+** button in Schematic **Hierarchy** offers **Schematic** and **Text
SPICE Schematic**. Text SPICE schematics use a notebook-text icon, appear in the
schematic tab bar with a `.sp` extension, and open a plain-text SPICE editor in
place of the canvas and canvas toolbar. Their source is stored directly in the
project and saved with it. They are not
projected into generated hierarchy symbols, but they can be selected as the
source for a simulation. For a text schematic, simulation uses the stored SPICE
source minus analysis-control directives such as `.tran`, `.dc`, `.ac`, `.op`,
and `.end`. The **Build Netlist** button and the schematic signal picker are
hidden for text schematics; only **Run Simulation** is shown, and it runs
directly without a separate build step.

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
column contains **Properties** and **Layers**. Layout entries use a notebook-pen
icon. Each layout node lists its child layout/device instances, planned
devices, rectangles, polygons, paths, and texts directly, with a distinct icon
for each entry type. Right-click an entry and choose **Rename…** to rename a
layout, script, instance, planned device, or shape. Right-click a layout entry
and toggle **Flatten Layout for DRC** to control whether **Run DRC** exports the
flattened instance geometry instead of only the layout cell's own shapes.
Right-clicking a schematic in the Schematic **Hierarchy** tab offers **Make
Layout from Schematic**. This creates a layout with the schematic's technology
and projects every schematic instance into a movable **Planned Device** box
under the new layout. Nets are drawn as straight connections between the boxes;
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
Python script editor in the middle view, and contain a generated static layout
as their child. The canvas tool columns are hidden while a procedural layout
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
and deleted directly in the canvas. PCell parameter names, values, and
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
`Activ` and `Poly2` to `Metal1` when those layers exist. Shapes on layers that do
not belong to the stack are reported in the status bar instead of being traced.

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
object rather than as independent segment rectangles. Select a path to show its
point handles. Drag a handle to move a point, double-click a segment to insert a
point, and right-click a handle to delete it. Changing layers while drawing
keeps the same path and records the layer for each segment; changing the width
starts a new path at the current endpoint. Corners and endpoints are square,
not rounded. Layer-change vias are stored with the path and move with it.
Segments adjacent to a via extend by half the via size so the copper fully
overlaps the via on both layers.

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

The **Draw Text** tool places a text label on the currently selected layer.
Click the canvas, enter the string, and ICE creates a movable text object with
a 1 um nominal height. Text is stored in the project and exported as GDSII
TEXT records.

**Tools → Run DRC** exports the active layout cell to a temporary GDSII file,
runs `gdscheck` with the process deck derived from the cell's PDK, and parses
the generated `.lyrdb` report. Violations appear in the **DRC Report** tab of
the activity dock with severity, rule, message, cell, and location columns.

**Tools → Run LVS** compares the active layout against its source schematic.
ICE infers the schematic from planned-device links, then from matching layout
and schematic names, and asks for a schematic when the match is ambiguous. LVS
checks device presence, reference names, device types, shared parameters on
placed device instances, and net topology from planned connections. Device
parameters are compared numerically when both sides use compatible quantity
values, so `0.15u` and `150n` are equivalent. If the layout has no planned
connectivity, LVS reports a warning and compares devices only. Results appear in
the **LVS Report** tab with mismatch categories and schematic/layout object ids.
Text SPICE schematics are not supported by LVS yet.

The editor must support integer database coordinates and a clear display-unit
conversion. It should never use display precision as a substitute for database
precision.

Generated layout should enter the same editor and database as manually created
layout. The user should be able to inspect generated provenance, edit geometry,
and continue with Python or agent operations without exporting to an
intermediate format.

## Simulation scripts

The Simulation panel **+** menu offers **Add Simulation…** and **Add Simulation
Script…**. Simulation scripts are project-level Python files under `scripts/`,
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
