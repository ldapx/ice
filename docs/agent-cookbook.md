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

- Mixed mode currently supports top-level HDL instances only.
- `logic` scalar ports are thresholded at `logic_threshold` (default 0.5 V) and
  outputs drive `output_high`/`output_low` (default 1 V / 0 V).
- `real` ports are bridged without thresholding.
- Buses need `bus_mode="flatten"`; pins become `data[0]`, `data[1]`, …
- A digital trace is named `<instance>.<port>` (for example `U1.pwm_out`).
  Do not prefix it with `v(...)`; `v(...)` is only for analog nodes.
- Mixed simulations with default timing use a 1 µs stop and 1 ns step. Set an
  explicit stop/step for slower RC filters or faster clocks.

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
| `mixed-signal digital input node ... not returned` | ngspice did not probe the input net. | Check the net is connected and not named only as ground. |
| `v(U1.port)` hint | You requested analog syntax for a digital trace. | Use `U1.port` in graph/trace calls. |
