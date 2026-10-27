# ICE desktop UI modules

`ice` is split into small translation units so presentation code does not keep
growing in one file.

- `main.cpp` — process entry point, Qt application setup, project-file argument.
- `app_window.*` — `AppWindow` interface and the `DocumentWindow` shell: menus,
  toolbars, document tabs, commands, and project lifecycle.
- `app_widgets.*` — startup view, empty-cell cards, status bar, command palette,
  activity/diagnostics panels, and the create-layout dialog.
- `layout_canvas.*` — layout canvas interaction, selection, movement, and device
  placement preview.
- `schematic_canvas.*` — schematic canvas interaction, wiring, symbol movement,
  and signal-pick mode.
- `layout_devices.*` — bundled layout-device library, device browser, device
  preview, layout/PyCell helpers, and geometry generation.
- `procedural_layout.*` — Python script editor and runner, PDK layout-extension
  dispatch, and generated-layout refresh for procedural layouts.
- `properties_panel.*` — the shared schematic/layout properties panel.
- `side_panels.*` — project/layout hierarchy, layers, simulations, symbols, and
  schematic netlist panels.
- `text_document_editor.*` — source-text editor used by text SPICE schematic
  tabs.
- `schematic_utils.*` — generated hierarchical schematic symbol artwork.
- `ui_helpers.*` — shared icons, headings, layer styles, transforms, and cursor
  guards.
- `symbol_ui.*`, `package_ui.*`, `simulation_plot.*`, `icons.*`, and
  `appearance.*` — reusable dialogs, symbol/package libraries, plotting, icons,
  and theme handling.
