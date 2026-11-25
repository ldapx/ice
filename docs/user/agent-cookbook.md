# ICE Agent Cookbook

Practical recipes for agents using `ice-mcp` and `ice-cli`. For the full tool
catalog, use `ice_help(topic=...)` or `ice-cli --help`.

## Open an existing project

MCP:

```text
project_summary()
project_open()                 # uses cwd/parent search when one candidate exists
project_open(path="/work/design/project.icepro")
```

`project_summary` returns `open:false`, candidate paths, and suggested calls
when no project is loaded. Set `ICE_PROJECT` or pass `path` if more than one
project exists nearby.

CLI:

```sh
ICE_PROJECT=/work/design/project.icepro ice-cli project-info
ice-cli project-info /work/design/project.icepro
```

## Build a small drawn RC circuit

MCP:

```text
schematic_create(name="rc")
schematic_add_circuit(
  cell="rc",
  components=[
    {"type":"voltage source","name":"V1","parameters":{"value":"PULSE(0 1 1n 1n 1n 10n 20n)"}},
    {"type":"resistor","name":"R1","parameters":{"resistance":"1k"}},
    {"type":"capacitor","name":"C1","parameters":{"capacitance":"1n"}}
  ],
  nets=[
    {"name":"in","pins":["V1.+","R1.1"]},
    {"name":"out","pins":["R1.2","C1.1"]},
    {"name":"0","pins":["V1.-","C1.2"]}
  ]
)
schematic_validate(schematic="rc")
simulation_create(name="rc_tran", schematic="rc", template="tran")
simulation_run(simulation="rc_tran")
simulation_trace(simulation="rc_tran", signals=["v(out)"], max_points=2000)
```

CLI equivalent:

```sh
ice-cli schematic-add-cell design top
ice-cli schematic-add-circuit design top circuit.json
ice-cli schematic-validate design top
ice-cli simulation-add design rc_tran top transient 1u 1n v(out)
ice-cli simulation-run design rc_tran
ice-cli simulation-trace design rc_tran v(out) 2000
```

`circuit.json`:

```json
{
  "components": [
    {"type": "voltage source", "name": "V1",
     "parameters": {"value": "PULSE(0 1 1n 1n 1n 10n 20n)"}},
    {"type": "resistor", "name": "R1", "parameters": {"resistance": "1k"}},
    {"type": "capacitor", "name": "C1", "parameters": {"capacitance": "1n"}}
  ],
  "nets": [
    {"name": "in", "pins": ["V1.+", "R1.1"]},
    {"name": "out", "pins": ["R1.2", "C1.1"]},
    {"name": "0", "pins": ["V1.-", "C1.2"]}
  ]
}
```

## Parameterized schematic cells

A drawn schematic cell can declare parameters. Component values use SPICE
`{name}` expressions, and a hierarchical block instance overrides matching
parameters:

```python
schematic_create(name="divider")
schematic_add_circuit(
    cell="divider",
    components=[
        {"type": "input", "name": "A"},
        {"type": "output", "name": "Y"},
        {"type": "resistor", "name": "R1", "parameters": {"resistance": "{R}"}},
        {"type": "capacitor", "name": "C1", "parameters": {"capacitance": "{C}"}},
    ],
    nets=[
        {"name": "in", "pins": ["A.IN", "R1.1", "C1.2"]},
        {"name": "out", "pins": ["R1.2", "C1.1", "Y.OUT"]},
    ],
)
schematic_set_cell_parameter(cell="divider", name="R", value="1k")
schematic_set_cell_parameter(cell="divider", name="C", value="1n")

schematic_create(name="top")
schematic_add_symbol(cell="top", name="divider", source_cell="divider")
schematic_add_instance(cell="top", symbol="divider", name="X1", x=0, y=0)
schematic_set_parameter(cell="top", instance="X1", name="R", value="2k")
```

The child netlist is emitted with `.subckt divider A Y params: R=1k C=1n` and
`R1 A Y {R}`. The parent instance is emitted as
`X1 in out divider R=2k C=1n`; parameters not overridden keep their defaults.
Use a `pin` component (the bidirectional `Pin` symbol) instead of `Input` or
`Output` when a port can drive or receive, and connect its `PIN` pin.
A top-level cell parameter becomes `.param name=value`. CLI equivalents are
`schematic-set-cell-parameter <project> <cell> <name> <value>` and
`schematic-set-parameter <project> <cell> <instance> <name> <value>`.
`schematic-remove-cell-parameter` removes a declaration.

## Compact analog procedural layout

Use composition and constraints rather than manual `move` loops with fixed
gaps. First inspect the live API and the PDK device names, then validate the
script before regeneration:

```text
layout_api(pdk="sky130A")
layout_api(name="device")
layout_api(name="nfet")
layout_api(class_name="Layout")
layout_validate_script(script="...", pdk="sky130A", layout="ota")
```

A compact two-row floorplan looks like this:

```text
layout_create_procedural(name="ota", pdk="sky130A")
layout_set_script(layout="ota", script='''
layout.plan(devices=["M1", "M2", "M3", "M4"], rows=2,
            target_utilization=0.20, max_gap_um=10,
            require_composition=True)

M1 = layout.device("nfet", name="M1", w="3u", l="2u",
                   nets={"G": "INN", "D": "N1", "S": "TAIL", "B": "VSS"})
M2 = layout.device("nfet", name="M2", w="3u", l="2u",
                   nets={"G": "INP", "D": "N2", "S": "TAIL", "B": "VSS"})
M3 = layout.device("pfet", name="M3", w="5u", l="2u",
                   nets={"G": "N1", "D": "N1", "S": "VDD", "B": "VDD"})
M4 = layout.device("pfet", name="M4", w="5u", l="2u",
                   nets={"G": "N1", "D": "N2", "S": "VDD", "B": "VDD"})

top = layout.row(gap=0, align="bottom")
top.add(M3)
top.add(M4)

bottom = layout.row(gap=0, align="top")
bottom.add(M1)
bottom.add(M2)

core = layout.column(gap=layout.um(2), align="center")
core.add(top)
core.add(layout.generate("rail", layer="met1.drawing",
                         width=top.width, height=layout.um(2), net="VSS"))
core.add(bottom)
layout.add(core)

layout.constraint("Boundary", max_width=layout.um(120), max_height=layout.um(120))
layout.finish_placement()
layout.review(require_plan=True)
layout.report(print_report=True)
''')
layout_regenerate(layout="ota")
layout_review(layout="ota", require_plan=True)
```

`layout.finish_placement()` is a hard gate: with a declared plan, `route()` and
`connect()` reject calls until the utilization, row-gap, row-count, and
composition requirements pass. `layout_regenerate` returns structured
`layout_quality`, `layout_plan`, and `layout_review` objects; use those directly
instead of parsing `generation_log`. If `utilization` is low or `large_gaps`
contains unexplained rows, compact the floorplan before routing. Add spacing only
after a router/DRC failure, not between every device by default. Use
`layout.repeat` and `layout.symmetric` for matched pairs, `layout.grid` for unit
arrays, and `layout.generate("mos_pair")` or `layout.generate("cap_array")` where
they fit.

## Text SPICE for simple analog circuits

Use text SPICE when the circuit is flat and you do not need a drawn canvas:

```text
schematic_create(name="rc_text", kind="text", pdk="ihp-sg13g2",
                 text="* RC\nV1 in 0 PULSE(0 1 1n 1n 1n 10n 20n)\nR1 in out 1k\nC1 out 0 1n\n.end\n")
```

ICE applies the package PDK setup automatically from the cell PDK. Do not copy
another PDK's `.include`/`.lib` lines into the source. `simulation_run` returns
the applied `pdk_setup` so you can verify what ICE injected.

You can also import a SPICE file:

```text
schematic_import_spice(name="rc_import", mode="drawn", file="rc.sp")
schematic_import_spice(name="rc_text", mode="text", file="rc.sp")
```

`mode="auto"` uses the drawn importer only when the source is a flat supported
R/C/L/V/I/D netlist.

## Mixed-signal PWM DAC

For a PWM DAC, use a **logic PWM output** into an RC low-pass. Do not convert the
PWM to `real` just to cross the analog boundary; the mixed bridge already maps
logic outputs to 0/1 PWL sources and samples analog input nodes through a
threshold.

```text
schematic_create_systemverilog(
  name="pwm",
  text="module pwm(input logic clk, output logic pwm_out);\n"
       "    logic [2:0] counter = 3'd0;\n"
       "    always_ff @(posedge clk) counter <= counter + 1'b1;\n"
       "    assign pwm_out = counter[2];\n"
       "endmodule\n"
)
schematic_create(name="pwm_dac")
schematic_add_hdl_instance(cell="pwm_dac", source_cell="pwm", name="U1", x=0, y=0)
schematic_add_component(cell="pwm_dac", type="voltage source", name="VCLK",
                        parameters={"value":"PULSE(0 1 5n 1n 1n 10n 20n)"})
schematic_add_component(cell="pwm_dac", type="resistor", name="R1",
                        parameters={"resistance":"1k"})
schematic_add_component(cell="pwm_dac", type="capacitor", name="C1",
                        parameters={"capacitance":"10n"})
schematic_add_net(cell="pwm_dac", name="clk", pins=["U1.clk","VCLK.+"])
schematic_add_net(cell="pwm_dac", name="pwm", pins=["U1.pwm_out","R1.1"])
schematic_add_net(cell="pwm_dac", name="out", pins=["R1.2","C1.1"])
schematic_add_net(cell="pwm_dac", name="0", pins=["VCLK.-","C1.2"])
schematic_validate_hierarchy(schematic="pwm_dac")
simulation_create(name="pwm_sim", schematic="pwm_dac", template="pwm_dac",
                  stop=100e-6, step=10e-9)
simulation_run(simulation="pwm_sim")
simulation_trace(simulation="pwm_sim", signals=["v(out)","U1.pwm_out"], max_points=4000)
```

This 50% PWM example settles near 0.5 V for a 1 V logic high and an RC time
constant of 10 µs. For a duty-controlled DAC, expose `duty` as a bus and create
the instance with `bus_mode="flatten"`, then wire `U1.duty[0]` … individually.

Notes:

- HDL instances may be top-level or nested inside drawn subcells; nested blocks
  receive scoped instance names such as `U1_U2`.
- `logic` scalar ports are thresholded at `logic_threshold` (default 0.5 V) and
  outputs drive `output_high`/`output_low` (default 1 V / 0 V).
- `real` ports are bridged without thresholding.
- Buses need `bus_mode="flatten"`; pins become `data[0]`, `data[1]`, …
- A digital trace is named `<instance>.<port>` (for example `U1.pwm_out`).
  Do not prefix it with `v(...)`; `v(...)` is only for analog nodes.
- Mixed simulations with default timing use a 1 µs stop and 1 ns step. Set an
  explicit stop/step for slower RC filters or faster clocks.

## Digital RTL-to-GDS chip

Use SystemVerilog source cells for RTL and testbenches. A source cell may
contain both; the module with `$dumpfile`/`$dumpvars` becomes the simulation
top.

```text
schematic_create_systemverilog(
  name="counter",
  text="module counter(input logic clk, input logic rst, output logic [3:0] q);\n"
       "  always_ff @(posedge clk) q <= rst ? 4'd0 : q + 1'b1;\n"
       "endmodule\n"
)
schematic_create_systemverilog(
  name="counter_tb",
  text="`timescale 1ns/1ps\n"
       "module counter_tb;\n"
       "  logic clk = 0, rst = 1; logic [3:0] q;\n"
       "  counter dut(clk, rst, q);\n"
       "  always #5 clk = ~clk;\n"
       "  initial begin $dumpfile(\"counter.vcd\"); $dumpvars(0, counter_tb);\n"
       "    #20 rst = 0; #200 $finish; end\n"
       "endmodule\n"
)
simulation_create(name="counter_sim", schematic="counter_tb")
simulation_run(simulation="counter_sim")
implementation_create(
  name="counter_impl", top="counter", pdk="sky130",
  source_cells=["counter"], utilization=0.35, timing_driven=true
)
implementation_run(run="counter_impl")
implementation_import_layout(run="counter_impl")
implementation_export_gds(run="counter_impl")
# Create a layout, then instance "counter_impl device" from Layout > Devices.
```

`implementation_run` runs synthesis through final and maps the netlist
automatically before floorplan. The final stage also attempts a KLayout GDS
merge when the PDK provides standard-cell GDS decks. Inspect
`implementation_status` for stage states, artifacts, timing, power, route DRC,
and antenna metrics; use `implementation_log` for the failing stage first.
`implementation_update(from="floorplan", utilization=0.5)` changes settings and
invalidates the affected checkpoints.

## Corners, sweeps, and measurements

`simulation_update` accepts analysis settings, corners, sweeps, Monte Carlo
options, and declarative measurements. Use `simulation_corner_defaults` first
to load the PDK's TT/FF/SS sections, then add any supply or parameter overrides:

```python
simulation_corner_defaults(simulation="rc_tran")
simulation_update(
    simulation="rc_tran",
    temperature=27,
    corners=[
        {"name": "tt", "temperature": 27},
        {"name": "ff", "temperature": 85, "source_overrides": {"V1": 1.1}},
    ],
    sweeps=[{"parameter": "temperature", "values": [27, 85]}],
    monte_carlo={"enabled": True, "samples": 20, "temperature_sigma": 2.0,
                 "supply_sigma_percent": 5.0, "supply_source": "V1",
                 "supply_nominal": 1.0, "seed": 7},
    measurements=[
        {"name": "gain", "metric": "gain", "signal": "out", "input": "in"},
        {"name": "settling", "metric": "settling_time", "signal": "out"},
        {"name": "power", "metric": "power", "signal": "i(v1)", "voltage": 1.0},
    ],
)
experiment = simulation_experiment(simulation="rc_tran", store=False)
for point in experiment["points"]:
    print(point["variant"], point["state"], point["measurements"])
```

`simulation_measure` also evaluates a one-off metric with
`metric="slew_rate"`, `"bandwidth"`, `"phase_margin"`, or `"power"` without
storing a new measurement on the simulation.

## Waveform analysis

Graphs are interactive and store presentation analysis in the project:
`simulation_graph_update` accepts `view="db"`/`"phase"`, `fft`, math channels,
and overlays. Use `simulation_runs` to find comparison runs and
`simulation_waveform` to read the analyzed data:

```python
runs = simulation_runs(simulation="rc_tran")
simulation_graph_update(
    simulation="rc_tran",
    graph="Graph 1",
    view="db",
    fft={"enabled": True, "window": "hann", "points": 1024},
    derived_traces=[{"name": "slope", "expression": "derivative(out)"}],
    overlays=[{"run_id": runs["runs"][0]["run_id"], "label": "tt"}],
    markers=[{"label": "M1", "signal": "out", "x": 1e-6, "y": 0.5}],
)
wave = simulation_waveform(
    simulation="rc_tran",
    signals="out",
    view="db",
    fft={"enabled": True, "window": "hann", "points": 1024},
    derived=[{"name": "slope", "expression": "derivative(out)"}],
)
```

Expressions support arithmetic and `abs`, `sqrt`, `exp`, `ln`, `log10`, `db`,
`mag`, `phase`, `derivative`, `integral`, `min`, and `max`. Internally the GUI
adds cursors A/B with delta readout, wheel/box zoom and pan, and measurement
annotations on the plot.

## PDK setup matrix

ICE applies the package PDK setup automatically. `pdk_info` is authoritative;
this table is a quick reference.

| PDK | Recommended entry | Notes |
| --- | --- | --- |
| IHP SG13G2 | `.lib ".../models/cornerMOSlv.lib" mos_tt` | Do not `.include` `cornerMOSlv.lib`; OSDI/PSP devices need `pdk_build`. |
| Sky130 | `.option scale=1u` + `.lib ".../sky130.lib.spice" tt` | Use the package ngspice root. |
| GF180MCU | `.include ".../design.ngspice"` + `.lib ".../sm141064.ngspice" typical` | Include the design preamble first. |

Do not copy one PDK’s include pattern into another PDK’s text schematic.
`schematic_validate` warns about manual `.include`/`.lib` lines in text cells,
and `simulation_run` returns the `pdk_setup` ICE actually applied.

## Validate before simulating

```text
schematic_validate(schematic="top")
schematic_validate_hierarchy(schematic="top")
```

`schematic_validate` checks:

- unconnected pins and single-pin nets,
- duplicate instance/net names,
- missing resistance/capacitance/source/model values,
- PDK availability and readiness,
- manual PDK include/lib lines in text SPICE,
- mixed-signal port/bit mapping.

`simulation_run` returns actionable hints and `suggested_calls` when it fails.
If a probe uses `v(U1.digital_out)`, switch to `U1.digital_out`.

## Delete and iterate

```text
schematic_delete(schematic="top", dry_run=true)
schematic_delete(schematic="top", cascade=true)
```

The dry run lists dependent simulations and hierarchical symbols. Cascade removes
them and cleans the removed HDL symbol pins from parent nets. Use
`project_undo()` / `project_redo()` for the last project mutation.

## Common error catalog

| Error/code | Meaning | Fix |
| --- | --- | --- |
| `schematic.pin.unconnected` | A symbol pin is not in any net. | `schematic_add_net` or remove the unused pin/instance. |
| `schematic.net.single` | A net has fewer than two pins. | Connect the other end or delete the net. |
| `schematic.component.value` | R/C/L/source has no value. | `schematic_set_parameter` with `resistance`, `capacitance`, `inductance`, or `value`. |
| `schematic.symbol.pins` | A symbol has no pins; it cannot be wired or drawn usefully. | Recreate it with `schematic_add_component` or `schematic_add_hdl_instance`, or add pins explicitly. |
| `schematic.symbol.artwork` | A symbol has no artwork and renders as a white placeholder. | Recreate it with `schematic_add_component`; known names auto-load library artwork. |
| `schematic.pdk.missing` | The cell PDK is not installed. | `pdk_info`, install the ICE package, then `pdk_build` if OSDI is needed. |
| `schematic.spice.pdk_manual` | Text SPICE contains manual `.include`/`.lib` setup. | Remove it; ICE applies the package setup from the cell PDK. |
| `hdl.mixed` | Mixed port/bit mapping is invalid. | Run `schematic_validate_hierarchy`; use `bus_mode="flatten"` or fix pin names. |
| `simulation.result` | No stored run is available. | Run `simulation_run` without an external-only output path, then retry. |
| `implementation.stage` | A Yosys/OpenROAD stage failed or a required artifact is missing. | Call `implementation_log` for that stage, fix the first actionable error, then rerun from the stage. |
| `implementation.gds` | KLayout, standard-cell GDS decks, or the PDK DEF-to-GDS layer map are unavailable. | Run `implementation_toolchain`; install KLayout or fix the PDK/ORFS GDS and `layer_map` paths, then call `implementation_export_gds`. |
| `drc.lyrdb` | The DRC report could not be parsed. | Check `drc_run` `log` and rerun with the package `technology/drc.json` deck or a valid gdscheck process deck. |
| `mixed-signal digital input node ... not returned` | ngspice did not probe the input net. | Check the net is connected and not named only as ground. |
| `v(U1.port)` hint | You requested analog syntax for a digital trace. | Use `U1.port` in graph/trace calls. |
