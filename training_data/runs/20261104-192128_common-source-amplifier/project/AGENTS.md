# project

This directory is an ICE project.

- `project.icepro` contains project metadata.
- `sch/*.icesch` contains schematic cells.
- `lay/*.icelay` contains layout cells and procedural layouts.
- `sim/*.icesim` contains simulations.
- `scripts/*.py` contains project-level simulation scripts.
- `build/` contains generated simulation results.

When the user asks you to make a schematic or circuit, use this typical workflow:

1. Clarify the expectations (waveforms, gain, bandwidth, settling, power, etc.).
2. Create or update the schematic.
3. Create the simulation and choose the signals to capture.
4. Create graphs that show the signals needed to check the expectations.
5. Run the simulation and create/run a simulation script that compares the results with the expectations and can generate GUI graphs.
6. If the expectations are not met, tweak the schematic and continue the loop.

For scripted work, use `ice-cli` or `ice-mcp`. Successful `ice-mcp` project mutations are saved to disk automatically.
Run stored simulations with `ice-cli simulation-run <project> <simulation>`; its run is written under `build/` and loaded by the GUI graph tabs.

Use the ICE GUI, `ice-cli`, `icepy`, or `ice-mcp` to inspect and edit the project.
