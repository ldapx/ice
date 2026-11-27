# project

This directory is an OpenVLSI project.

- `project.ovpro` contains project metadata.
- `sch/*.ovsch` contains schematic cells.
- `lay/*.ovlay` contains layout cells and procedural layouts.
- `sim/*.ovsim` contains simulations.
- `scripts/*.py` contains project-level simulation scripts.
- `build/` contains generated simulation results.

When the user asks you to make a schematic or circuit, use this typical workflow:

1. Clarify the expectations (waveforms, gain, bandwidth, settling, power, etc.).
2. Create or update the schematic.
3. Create the simulation and choose the signals to capture.
4. Create graphs that show the signals needed to check the expectations.
5. Run the simulation and create/run a simulation script that compares the results with the expectations and can generate GUI graphs.
6. If the expectations are not met, tweak the schematic and continue the loop.

For scripted work, use `openvlsi-cli` or `openvlsi-mcp`. Successful `openvlsi-mcp` project mutations are saved to disk automatically.
Run stored simulations with `openvlsi-cli simulation-run <project> <simulation>`; its run is written under `build/` and loaded by the GUI graph tabs.

Use the OpenVLSI GUI, `openvlsi-cli`, `openvlsipy`, or `openvlsi-mcp` to inspect and edit the project.
