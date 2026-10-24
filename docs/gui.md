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

## Document and project lifecycle

The GUI should support:

- creating a project;
- opening and closing projects;
- opening schematic, layout, simulation, and result documents;
- tracking unsaved changes;
- explicit save and save-as operations;
- reporting missing packages, PDKs, or incompatible schema versions;
- recovering from failed external jobs without losing in-memory state.

Project state, view state, and transient job state should be separate:

| State | Examples | Persistence |
| --- | --- | --- |
| Project state | cells, nets, geometry, parameters | project files |
| View state | zoom, panes, selections, visibility | optional UI settings |
| Job state | running simulation, progress, cancellation | runtime and result records |

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

The editor must support integer database coordinates and a clear display-unit
conversion. It should never use display precision as a substitute for database
precision.

Generated layout should enter the same editor and database as manually created
layout. The user should be able to inspect generated provenance, edit geometry,
and continue with Python or agent operations without exporting to an
intermediate format.

## Theme palette

The application palette is editable from **Settings → Theme Palette…** and is
persisted with `QSettings`. **Settings → Theme → Dark Theme / Light Theme**
applies either preset. The schematic canvas, layout canvas, and simulation
plots use the same color as the application background (`background-2`).

| Role | Dark preset | Light preset |
| --- | --- | --- |
| `accent-1` | `#fff47e` | `#b8860b` |
| `accent-2` | `#fffde2` | `#a88f00` |
| `accent-3` | `#6afdff` | `#008b8b` |
| `accent-4` | `#828aff` | `#3f51b5` |
| `accent-5` | `#9a16ff` | `#7a00cc` |
| `accent-6` | `#bc4fff` | `#9c27b0` |
| `background-1` | `#272730` | `#f2ead4` |
| `background-2` | `#19191c` | `#fffde2` |
| `surface-1` | `#31313c` | `#f7f0dc` |
| `border-1` | `#464652` | `#c8b98a` |
| `text-1` | `#fffde2` | `#272730` |
| `text-2` | `#a8a8b3` | `#6b6b76` |
| `text-on-accent` | `#19191c` | `#fffde2` |
| `grid-1` | `#2a2a32` | `#e8e0c8` |
| `grid-2` | `#383842` | `#cfc6aa` |
| `axis-1` | `#9a9aa6` | `#272730` |

All roles can be changed independently, and **Reset Theme** restores the
current dark/light preset defaults.

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
