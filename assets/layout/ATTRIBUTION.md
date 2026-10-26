# Layout device library attribution

`ihp.sg13g2.json` contains layout geometry derived from the IHP SG13G2
open-source PDK:

- Project: IHP-Open-PDK
- Source: https://github.com/IHP-GmbH/IHP-Open-PDK
- Revision: `7d92ceafebfc51abb3285570ca0380c39e7e1826` (`v2.0.0-44-g7d92cea`)
- License: Apache-2.0

The geometry in this file was extracted from the GDS files under
`ihp/gds/` in the upstream repository and flattened into ICE layout cell
polygons for the Layout editor's device browser. For cells with a PyCell
generator, the stored geometry and parameter metadata were regenerated from
the matching implementation under `ihp/refs/ihp_pycell/` using the repository's
pure-Python CNI compatibility layer. Parameter names, defaults, and
descriptions follow the PyCell parameter declarations. The same PyCell layer is
used at runtime when a parameter value is edited.
