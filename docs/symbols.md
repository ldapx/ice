# Schematic symbol library

The editor loads its built-in symbol library from the `symbols/` directory in
the repository: one `<name>.toml` sidecar plus its sibling `<name>.svg`
artwork file per symbol. Most SVG artwork is extracted from the
[SchematicSymbolsSVG](https://github.com/sjgallagher2/SchematicSymbolsSVG)
collection (MIT, (c) 2023 Sam Gallagher); the waveform source, current source,
and inductor symbols are original ICE artwork. See `symbols/ATTRIBUTION.md` for
sources and modifications. No symbol artwork is hardcoded in the application.

Use **Project → Place Device** or press `P` in the editor. The device browser
groups devices into category tabs, supports name/category search, and shows an
SVG symbol preview plus the symbol's default parameters. Symbols are stored in
the canonical schematic model with stable IDs and explicit pin IDs, so
placement, movement, deletion, and connections survive project save/reload.

## Symbols

| Symbol | Source element | Pins |
| --- | --- | --- |
| Resistor | `orr_resistor_zigzag` | 1, 2 |
| Capacitor | `orr_cap_nonp` | 1, 2 |
| Inductor | original ICE artwork | 1, 2 |
| Diode | `orr_diode2` | A, K |
| Voltage Source | `orr_battery` | +, - |
| Pulse Voltage Source | original ICE artwork | +, - |
| Sine Voltage Source | original ICE artwork | +, - |
| Current Source | original ICE artwork | +, - |
| Ground | `orr_gnd` | GND |
| Op Amp | `orr_opamp2` | +, -, OUT |
| NMOS | `orr_nmos` | G, D, S, B |
| Input | original ICE artwork | IN |
| Output | original ICE artwork | OUT |

Pin coordinates are integers in symbol units with the origin at the artwork
center, so the GUI can center artwork of any size on the symbol origin and
wires attach exactly at the artwork terminals.

## Sidecar format

```toml
[symbol]
name = "resistor"
artwork = "resistor.svg"
# Or reference ICE's shared artwork from a package sidecar:
# artwork = "generic:resistor.svg"
category = "Passive"

[[pin]]
name = "1"
x = -16
y = 0
direction = "inout"

[[parameter]]
name = "resistance"
value = "1k"
```

`artwork` normally names a sibling SVG file. Package sidecars may instead use
`artwork = "generic:<file>.svg"` to reference ICE's built-in symbol artwork
directory. Only a plain SVG filename is accepted after `generic:`; absolute
paths and parent-directory traversal are rejected.

`direction` accepts `input`, `output`, or `inout`; the physical pin side is
derived from the pin position. The loader (`ice::SymbolLibrary::load`) rejects
a sidecar whose artwork file is missing or unsafe (not an SVG document,
embedded scripts, external references), whose pin names are empty or
duplicated, whose coordinates are not integers, or whose symbol name
duplicates another file. Optional `[[parameter]]` entries seed default instance
parameters whenever the symbol is placed; users can edit them per instance in
the properties editor.

## PDK symbol libraries

Each schematic stores the PDK it uses. The built-in Generic PDK exposes only
the generic library described above. A schematic linked to a package PDK also
loads that package's own `symbols/` directory, so the combined browser shows
generic symbols and PDK-specific symbols together. The IHP SG13G2, Sky130, and
GF180MCU packages provide NMOS/PMOS, bipolar, passive, and diode definitions and
merge them into the generic transistor, passive, and semiconductor categories.
Their sidecars reference ICE's shared artwork with `generic:` values, so the
packages do not duplicate SVG files. There are no built-in IHP, Sky130, or
GF180MCU symbol definitions in ICE.

## Source symbols

The source symbols share the same two-pin vertical layout and are distinguished
in the netlist by their symbol name and instance parameters:

- **Voltage Source** — constant DC value from the `voltage` parameter.
- **Pulse Voltage Source** — `initial_voltage`, `pulsed_voltage`,
  `delay_time`, `rise_time`, `fall_time`, `pulse_width`, `period`, and
  `pulse_count`; emitted as `PULSE(v1 v2 td tr tf pw per ncycles)`. A
  `pulse_count` of `0` means a continuous pulse train.
- **Sine Voltage Source** — `offset_voltage`, `amplitude`, `frequency`,
  `delay_time`, `damping_factor`, and `phase`; emitted as
  `SIN(vo va freq td theta phi)`.
- **Current Source** — constant DC value from the `current` parameter.

The waveform sources also accept the short SPICE aliases (`v1`, `v2`, `td`,
`tr`, `tf`, `pw`, `per`, `vo`, `va`, `freq`, `theta`, `phi`) when reading
existing projects or hand-edited parameter tables. The pulse count also accepts
`number_of_pulses`, `cycles`, or `ncycles`.

Each symbol seeds those parameters when placed, and they can be edited per
instance in the properties dialog. A parameter named `value` is emitted
verbatim as a raw SPICE source expression and overrides the structured fields.

## Lookup

At startup the GUI searches `symbols/` next to the executable, the
compile-time source directory (so development builds work from any working
directory), the current working directory, and `/usr/share/ice/symbols`.
Placing a symbol without a resolvable library is an explicit error, not a
silent fallback.
