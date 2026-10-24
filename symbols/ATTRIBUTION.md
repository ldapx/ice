# Symbol artwork attribution

Most `*.svg` files in this directory are copies of artwork from the
[SchematicSymbolsSVG](https://github.com/sjgallagher2/SchematicSymbolsSVG)
collection, extracted from `Symbols-Inkscape.svg` (`<symbol>` elements). The
pulse voltage, sine voltage, current source, and inductor symbols are original
ICE artwork.

Source collection license (MIT, Copyright (c) 2023 Sam Gallagher):

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.

## Sources

| File | Source element | Modifications |
| --- | --- | --- |
| `resistor.svg` | `orr_resistor_zigzag` | rotated to horizontal, rescaled |
| `capacitor.svg` | `orr_cap_nonp` | rotated to horizontal, rescaled |
| `diode.svg` | `orr_diode2` | rotated to horizontal, rescaled |
| `voltage_source.svg` | `orr_battery` | dropped `+`/mark glyphs, rescaled |
| `ground.svg` | `orr_gnd` | rescaled |
| `opamp.svg` | `orr_opamp2` | dropped part-number text, rescaled |
| `nmos.svg` | `orr_nmos` | rescaled |
| `inductor.svg` | original ICE artwork | — |
| `pulse_voltage_source.svg` | original ICE artwork | — |
| `sine_voltage_source.svg` | original ICE artwork | — |
| `current_source.svg` | original ICE artwork | — |

All symbols: embedded text labels removed (text must not be baked into
artwork), annotation glyphs removed, geometry uniformly rescaled for
schematic use. Pin positions live in the sibling `*.toml` sidecars, not in
the SVG files. The `Circuit_macros.svg` file from the same collection (LPPL)
is not used here.
