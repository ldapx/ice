# Physical Design UI Proposal

Status: in progress

Implemented in the first pass:

- Final stage page, preview paths, and run actions through Final.
- Physical artifact opening, with external handling for large/ODB files.
- Route DRC artifact recording and a DRC marker viewer.
- Preview truncation warning and asynchronous ODB preview loading.
- Layer, net-type, and instance-category visibility controls with fill/tap/
  endcap cells hidden by default.
- Stable ODB preview metadata for nets, instances, pins, modules, and layers.
- Canvas selection, Inspector, focus-net highlighting, find, rulers, labels,
  coordinate readout, and image export.
- Heat-map settings, legend, CSV loading, and export for power, pin, RUDY, and
  IR drop.
- Stage stepper with live status markers.
- Timing metric grouping, OpenROAD STA path report generation, clock-net list,
  and module hierarchy.
- Final layout import and an "Open in OpenROAD GUI" action.

This document compares ICE's Physical Design workspace with the OpenROAD GUI
layout and records the implementation plan. The goal is to keep ICE's guided
stage flow while adding the inspection tools users expect from a physical
design environment.

## Summary

ICE already has a good stage-oriented workflow:

- Floorplan, Placement, CTS, Routing, and Final checkpoints are explicit.
- Prerequisites are enforced.
- Stage settings, metrics, logs, and artifacts are stored with the run.
- The implementation backend is process-isolated and reproducible.

The main weakness is that the physical workspace currently behaves like a run
monitor with a static preview. OpenROAD's GUI is a design-inspection
environment: it exposes object-level display controls, selection and
cross-probing, DRC markers, timing paths, clock trees, and configurable heat
maps. ICE should preserve its stage workflow while adopting those inspection
patterns.

## OpenROAD GUI reference

OpenROAD uses a canvas-first layout:

- Center: full layout canvas.
- Left: `Display Control` dock.
- Right: tabbed docks for Inspector, Hierarchy Browser, Timing Report, DRC
  Viewer, Clock Tree Viewer, and Charts.
- Bottom: scripting and selection docks.
- Status bar: cursor coordinates, database units, and scale.

Relevant reference files:

- `src/gui/src/mainWindow.cpp`: dock arrangement and menus.
- `src/gui/src/displayControls.cpp`: layer, net, instance, blockage, track,
  shape, and miscellaneous visibility controls.
- `src/gui/src/layoutViewer.cpp`: selection, highlight groups, rulers, labels,
  focus nets, route guides, net tracks, module view, and image export.
- `src/gui/src/inspector.cpp`: selected-object properties and actions.
- `src/gui/src/drcWidget.cpp`: DRC report loading, marker categories, and
  marker selection/focus.
- `src/gui/src/timingWidget.cpp`: timing path tables, path details, timing
  cones, and focus nets.
- `src/gui/src/clockWidget.cpp`: clock tree views.
- `src/gui/src/chartsWidget.cpp`: slack histograms and endpoint reporting.
- `src/gui/src/heatMapSetup.cpp`: heat-map scale, range, grid, alpha, legend,
  and selected-only settings.
- `src/gui/src/renderThread.cpp`: background layout rendering and loading
  indication.

## Current ICE physical UI

ICE's physical workspace is built from `ImplementationPage` instances stacked
by `PhysicalDesignPage` in `src/ice/implementation_ui.cpp`:

- Left: Runs tree, optional Netlists explorer, a Display tree, and an Artifacts
  list.
- Center: interactive preview canvas with toolbar, layer/category filtering,
  selection, focus nets, DRC markers, annotations, heat-map settings, CSV
  loading, and truncation warning.
- Right: stage settings, Inspector, Metrics, DRC, Timing, Clock, Hierarchy, and
  Properties.
- The physical workspace contains Floorplan, Placement, CTS, Routing, and Final
  pages, selected through a stage bar and the Runs tree.

The stage gating and run actions are a strength and should remain. The preview
canvas and diagnostics are where the largest improvements are needed.

## Issues and improvements

### P0: correctness and data integrity

1. Add the Final stage to the physical workspace.
   - Add a Final page and stage stepper entry.
   - Run "from here" and "run all" through Final, not only Routing.
   - Preview `db/6_final.odb` and `def/6_final.def`.
   - Expose final Verilog, SPEF, SDC, DRC, timing, and power artifacts.

2. Make preview truncation explicit.
   - `ice-openroad odb-preview` has a global 250,000-shape limit.
   - The preview JSON already contains a `truncated` flag; ICE must surface it.
   - Prefer per-category shape budgets so a large PDN cannot consume the entire
     budget and hide signal routing.
   - Show a warning on the canvas and in the stage details when geometry is
     incomplete.

3. Record and expose `reports/route_drc.rpt`.
   - The routing backend writes it, but it is not in the stage output list.
   - Add it to artifacts and provide a DRC viewer or an external-open action.

4. Make physical artifacts openable.
   - Physical artifact lists currently have no double-click handler.
   - Open logs, CSVs, DEF, Verilog, and reports in read-only document tabs.
   - Add an "Open externally" option for very large files.

5. Fix heat-map UX.
   - "Power density (CSV)" currently does not render on the canvas.
   - Either render imported power CSV data or label export-only entries
     explicitly.
   - Add Pin, RUDY, and IR-drop export options; later add their canvas
     renderers.

6. Avoid blocking the UI while loading previews and exporting heat maps.
   - ODB preview currently waits synchronously for up to 120 seconds.
   - Heat-map export currently waits synchronously for up to 300 seconds.
   - Move these operations to asynchronous workers with cancel/progress.

### P1: inspection and debugging

7. Add OpenROAD-style display controls.
   - Layers with visibility, color swatch, and selectability.
   - Nets by type: signal, power, ground, clock, reset, tie-off, scan, analog.
   - Instances by category: stdcell, macro, fill, tap, endcap, antenna, pad.
   - Blockages, rows, tracks, pin markers, route guides, special routing.
   - Hide fill and physical-only cells by default.
   - Preserve the current zoom when toggling visibility.

8. Add object selection and cross-probing.
   - Hit-test shapes, instances, ports, nets, and DRC markers.
   - Show selected object properties in an Inspector tab.
   - Highlight connected instances and fanin/fanout nets.
   - Focus a net and optionally draw flightlines.
   - Cross-probe between the physical preview and schematic/layout cells.

9. Add useful canvas tools.
   - Fit, zoom in/out, zoom to selection, and find instance/net.
   - Rulers and labels with snapping.
   - Scale bar, cursor coordinates, and DBU readout.
   - Save visible/entire layout image.
   - Preserve zoom and pan across overlays and refresh operations.

10. Add diagnostics views.
    - DRC marker list with category/layer filters, click-to-zoom, and selection.
    - Setup/hold WNS/TNS, worst-path table, path detail, and timing cones.
    - Clock-tree viewer after CTS.
    - Slack histograms and endpoint reporting.

11. Improve heat maps.
    - Support Placement, Routing, Power, Pin, RUDY, and IR drop.
    - Configurable grid size, log/reverse scale, min/max range, alpha, legend,
      numeric labels, and selected-region-only.
    - Keep the legend visible and allow the overlay to sit above or below
      geometry.

12. Improve metrics and quality gates.
    - Group metrics by stage instead of one flat map.
    - Add units, display names, previous-stage deltas, and run-to-run compare.
    - Separate stdcell instance area from fill/physical cells.
    - Promote DRC, antenna, setup/hold, and power to pass/fail badges.
    - Add a stage summary card instead of a single hint label.

### P2: workflow and advanced visualization

13. Add a persistent stage stepper.
    - Floorplan → Placement → CTS → Routing → Final.
    - Show status, elapsed time, quality badges, and the next action.
    - Keep the run selector and Run/Cancel actions in one header.

14. Add a hierarchy/module browser.
    - Modules, instance counts, macro counts, area, and visibility/color.
    - Module-view coloring on the canvas.

15. Add a final-stage handoff.
    - Import final DEF/GDS into a layout cell.
    - Open SPEF, final Verilog, and SDC from the run.
    - Optionally launch the OpenROAD GUI on the active stage ODB as an
      inspection fallback.

16. Improve floorplan settings.
    - Validate site and layer names from the platform.
    - Expose explicit die origin and core area, not only width/height.
    - Add macro placement, core spacing, tapcell/PDN controls, and a
      utilization/die-area preflight estimate.

17. Improve structured stage progress.
    - Parse OpenROAD global-router, placement, CTS, and detailed-router
      progress lines.
    - Show sub-step, iteration, violation count, and elapsed time instead of
      the latest raw log line.

## Target layout

```text
[Run selector] [Floorplan] [Placement] [CTS] [Routing] [Final]
[Run stage] [Run from here] [Run all] [Cancel]     [DRC][Timing][Antenna] badges

+----------------------+--------------------------------------+-----------------------------+
| Runs / History       | Canvas                               | Settings | Inspector        |
| Display Controls     |   Fit / Select / Inspect / Measure   | Metrics  | DRC              |
|  Layers              |   Overlay + legend                   | Timing   | Clock            |
|  Nets                |                                      | Hierarchy|                  |
|  Instances           |                                      |                             |
|  Blockages / Tracks  |                                      |                             |
| Artifacts            | status: x,y µm | DBU | selection | scale|                             |
+----------------------+--------------------------------------+-----------------------------+
| Stage log with structured progress                                                     |
```

## Implementation status

- Phase 1: implemented.
- Phase 2: implemented for the physical preview, including display controls,
  selection, Inspector, focus nets, DRC markers, rulers/labels, image export,
  heat-map settings/CSV loading, and asynchronous preview/export.
- Phase 3: implemented as native summaries (timing metrics and OpenROAD STA
  path reports, clock nets, hierarchy, grouped metrics, Final layout import,
  SPEF/Verilog/SDC access) plus an **Open in OpenROAD GUI** bridge for full
  interactive timing-histogram, timing-cone, and clock-tree analysis.

Schematic cross-probing from a flattened gate-level physical netlist is not
meaningful without preserving source-cell provenance. The Inspector and focus
tools therefore use the ODB object identities directly.

## Immediate next steps

1. Preserve source-cell/module provenance in the synthesized netlist so
   physical selection can cross-probe back to schematic sources.
2. Add native timing histograms and timing cones on top of the STA report
   parser.
3. Replace the clock-net summary with a hierarchical clock-tree view using CTS
   buffer connectivity.
4. Add a native GDS export path for the Final stage.
