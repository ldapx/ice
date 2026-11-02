# Plan: Make ICE PDK Packages Independent of Volare/CIEL

Date: 2026-11-01

Status: proposal — hard cutover

Decision: ICE PDK packages become the only supported simulation PDK source.
There is no runtime fallback to Volare, CIEL, `PDK_ROOT`, or `PDK`.

## 1. Goal

Make an installed ICE PDK package sufficient for schematic simulation without
requiring a Volare/CIEL installation.

A user should be able to:

1. install an ICE PDK package,
2. open an ICE project,
3. create and simulate a transistor-level circuit,
4. and get the same result on a machine that has no `~/.volare` or `~/.ciel`
   directory and no `PDK_ROOT`/`PDK` environment variables.

The package must provide the ngspice model stack, the correct model sections,
the required include paths, optional OSDI compilation metadata, and example
device names. Full GDS/DRC/LVS/PCell data can remain optional in a larger ICE
package or a package-managed payload.

## 2. Current state

### 2.1 What ICE packages contain today

Installed PDK packages under `~/.ice/packages` are currently metadata-only:

```text
~/.ice/packages/ihp.sg13g2   276 KB
~/.ice/packages/sky130       316 KB
~/.ice/packages/gf180mcu     276 KB
```

They contain:

```text
package.toml
symbols/
layout/device_generator.py
layout/dummy_fill.py
.github/logo.png
```

They do **not** contain the ngspice model files or Verilog-A sources.

### 2.2 Where simulation models actually come from

ICE currently discovers simulation PDKs by scanning Volare/CIEL and system
locations in `src/icelib/pdk.cpp`:

```text
~/.volare
~/.volare/volare
~/.ciel
/usr/share/pdk
/usr/local/share/pdk
/opt/pdk
```

The real IHP/Sky130/GF180 model trees are under those roots. This is why ICE
still depends on Volare/CIEL even when an ICE PDK package is installed.

### 2.3 Current problems

1. Package identity and PDK identity differ:
   - package name: `ihp.sg13g2`
   - schematic PDK id: `ihp-sg13g2`
   - Volare path id: `ihp-sg13g2`
2. The package manifest has no simulation contract:
   - no model root,
   - no model file,
   - no section/corner,
   - no includes/options,
   - no OSDI/Verilog-A information,
   - no example devices.
3. IHP's `.spiceinit` depends on `$PDK_ROOT` and `$PDK`, which are easy to
   misconfigure and conflict when switching PDKs.
4. `pdk_info --probe` uses the `ngspice` command-line executable, while ICE
   simulations use `libngspice` in-process. These two paths can diverge.
5. A global `~/.spiceinit` is a machine-wide side effect and cannot represent
   multiple PDKs or project-specific model stacks cleanly.
6. Full PDK trees are too large for ordinary Git packages:
   - Sky130A: 1.2–8.7 GB,
   - GF180MCU A: ~1.9 GB,
   - IHP SG13G2: ~874 MB.

### 2.4 Decision: hard cutover

There will be no compatibility path for Volare/CIEL PDKs. Once this plan is
implemented:

- ICE scans only ICE package roots for simulation PDKs.
- `~/.volare`, `~/.ciel`, `PDK_ROOT`, and `PDK` are not consulted by
  `pdk_info`, `ice_doctor`, `ice-cli`, `ice-mcp`, or the GUI.
- A missing ICE PDK package is a hard error with an install hint, not a fallback.
- Existing projects and package manifests are migrated once to the package-based
  PDK IDs. After migration, the ICE package is required.

This is intentional: a silent fallback would let two different model stacks
masquerade as the same PDK and reintroduce the reproducibility problems this
plan is meant to remove.

## 3. Scope

### In scope

- A simulation-focused PDK contract in `package.toml`.
- Package-relative model paths and setup lines.
- ICE-package-based PDK discovery for CLI, MCP, GUI, and `icepy`.
- Local OSDI build/cache support for Verilog-A PDKs such as IHP.
- Explicit ngspice initialization inside ICE instead of relying on a global
  `~/.spiceinit`.
- Model hashes/version metadata for reproducibility.
- Migration of the existing IHP, Sky130, and GF180 packages.
- A one-time migration of existing project PDK references.
- Hard failure when a required ICE PDK package is not installed.

### Out of scope

- Runtime fallback to Volare/CIEL, `PDK_ROOT`, `PDK`, or system PDK trees.
- Treating an external PDK installation as an alternate simulation source.
- Vendoring complete GDS/DRC/LVS/PCell data into every package.
- Shipping precompiled OSDI binaries for every platform.
- Proprietary PDK redistribution.

## 4. Proposed package contract

### 4.1 Package directory layout

```text
ihp.sg13g2/
├── package.toml
├── libs.tech/
│   └── ngspice/
│       ├── models/
│       │   ├── cornerMOSlv.lib
│       │   └── ...
│       └── .spiceinit
├── pdk/
│   └── verilog-a/
│   │   ├── psp103/
│   │   ├── r3_cmc/
│   │   └── ...
│   └── osdi/                 # generated locally, not committed
├── symbols/
├── layout/
└── docs/
```

Simulation files are package-relative. No absolute paths and no
`$PDK_ROOT`/`$PDK` references are stored in the package.

### 4.2 Manifest schema

Extend `[pdk]` with an ID and a `[pdk.simulation]` table.

```toml
[pdk]
id = "ihp-sg13g2"
display_name = "IHP SG13G2"
default_schematic = "ihp_top"

[pdk.simulation]
engine = "ngspice"
root = "libs.tech/ngspice"

model_file = "models/cornerMOSlv.lib"
section = "mos_tt"

includes = []
options = []

requires_osdi = true
osdi_dir = "pdk/osdi"
verilog_a = "pdk/verilog-a"
spiceinit = "libs.tech/ngspice/.spiceinit"

[pdk.simulation.devices]
nmos = "sg13_lv_nmos"
pmos = "sg13_lv_pmos"
```

Sky130:

```toml
[pdk]
id = "sky130A"
display_name = "SkyWater SKY130A"

[pdk.simulation]
engine = "ngspice"
root = "libs.tech/ngspice"

model_file = "sky130.lib.spice"
section = "tt"
options = [".option scale=1u"]

[pdk.simulation.devices]
nmos = "sky130_fd_pr__nfet_01v8"
pmos = "sky130_fd_pr__pfet_01v8"
```

GF180:

```toml
[pdk]
id = "gf180mcuA"
display_name = "GlobalFoundries GF180MCU A"

[pdk.simulation]
engine = "ngspice"
root = "libs.tech/ngspice"

model_file = "sm141064.ngspice"
section = "typical"
includes = ["design.ngspice"]

[pdk.simulation.devices]
nmos = "nfet_03v3"
pmos = "pfet_03v3"
```

### 4.3 Proposed C++ data model

Extend `include/icelib/packages.hpp`:

```cpp
struct PackagePdkSimulation {
    std::string engine = "ngspice";
    std::filesystem::path root;
    std::filesystem::path modelFile;
    std::string section;
    std::vector<std::filesystem::path> includes;
    std::vector<std::string> options;
    bool requiresOsdi = false;
    std::filesystem::path osdiDir;
    std::filesystem::path verilogA;
    std::filesystem::path spiceinit;
    std::map<std::string, std::string> devices;
};

struct PackagePdk {
    std::string id;                 // canonical PDK id, e.g. ihp-sg13g2
    std::vector<std::string> aliases;
    std::string displayName;
    std::string defaultSchematic;
    std::vector<PackageLayer> layers;
    std::optional<PackagePdkSimulation> simulation;
};
```

Parse these fields in `src/icelib/packages.cpp::readManifest`. Reject paths that
escape the package root (`..`, absolute paths) and unknown engines.

## 5. Runtime PDK resolution

### 5.1 Package roots

Add a non-Qt package-root resolver to `icelib`:

```text
$ICE_PACKAGE_ROOT
$HOME/.ice/packages
$XDG_DATA_HOME/ice/packages
```

The GUI keeps its configured install directory but should call the same
resolver rather than duplicating path logic. CLI and MCP use the environment
and default paths.

### 5.2 Resolution order

1. ICE package with a matching `[pdk].id` or alias.
2. Error with the missing package and suggested install command.

There is no external-root step. Return `source: "ice-package"` in `pdk_info`.
Remove the Volare/CIEL/system scan from the runtime resolver; it must not be used
even when those directories exist.

### 5.3 Canonical IDs and aliases

Normalize IDs for matching:

- lower-case,
- replace `.` with `-`,
- compare package `id`, aliases, package name, and directory name.

Examples:

```text
ihp.sg13g2       -> ihp-sg13g2
ihp-sg13g2       -> ihp-sg13g2
sky130A          -> sky130a
gf180mcuA        -> gf180mcua
```

Aliases are used only by the one-time project/package migration. New projects
store the canonical `[pdk].id`; aliases are not a permanent alternate PDK source.

Project schematics created with `ihp.sg13g2` must continue to resolve to the
IHP PDK package.

### 5.4 `pdk_info` and `ice_doctor`

Add fields:

```json
{
  "id": "ihp-sg13g2",
  "source": "ice-package",
  "package": "ihp.sg13g2",
  "package_version": "0.2.0",
  "model_hash": "sha256:...",
  "requires_osdi": true,
  "osdi_ready": false,
  "spiceinit": "ice-managed",
  "simulation_ready": false,
  "missing": ["OSDI models not built"],
  "recommended_setup": [
    ".lib \"<package>/libs.tech/ngspice/models/cornerMOSlv.lib\" mos_tt"
  ],
  "example_devices": {
    "nmos": "sg13_lv_nmos",
    "pmos": "sg13_lv_pmos"
  }
}
```

`simulation_ready` means:

- the model file exists,
- the requested section exists,
- all relative includes resolve,
- required OSDI files exist and are loadable,
- the probe succeeds when requested.

## 6. Simulation integration in ICE

### 6.1 Stop relying on `~/.spiceinit`

The current model assumes a global `~/.spiceinit` and `PDK_ROOT`/`PDK`
environment. This is fragile and cannot represent multiple packages.

Add package-derived initialization to the simulation request:

```cpp
struct SimulationRequest {
    // existing fields ...
    std::vector<std::filesystem::path> osdiFiles;
    std::vector<std::string> initCommands; // osdi, set, option, sourcepath
    std::vector<std::string> modelIncludes;
};
```

`NgspiceSimulator::run` should:

1. initialize libngspice,
2. apply `initCommands` with `ngSpice_Command` (for example `osdi "<path>"`),
3. build the circuit netlist from the schematic plus package setup lines,
4. run the analysis,
5. include the package/model identity in the result fingerprint.

This removes the requirement for `PDK_ROOT`, `PDK`, and `~/.spiceinit` during
ICE simulations.

### 6.2 Keep the probe consistent with ICE

`pdk_info --probe` currently runs the `ngspice` executable. Add or switch to a
probe that uses the same libngspice path as ICE, or run both and report:

```json
{
  "probe": {
    "cli_ok": true,
    "shared_library_ok": true,
    "summary": "ngspice loaded the model and biased an NMOS"
  }
}
```

A package must not be marked ready merely because the CLI works if ICE's
in-process simulator cannot load the same model.

### 6.3 Per-simulation model stack

The project should retain the PDK package ID and model hash. The existing
`ice.lock`/package dependency mechanism should record:

```json
{
  "pdk": "ihp.sg13g2",
  "pdk_version": "0.2.0",
  "pdk_model_hash": "sha256:...",
  "osdi_compiler": "openvaf-r 23.5.0",
  "osdi_abi": "0.5"
}
```

This makes a simulation reproducible after PDK updates.

## 7. OSDI build workflow

### 7.1 Local build, not committed binaries

Do not commit `.osdi` files. They are platform/ABI-specific.

Add:

```text
ice-pkg pdk-build <package-name>
ice-cli pdk-build <pdk-id>
```

The command:

1. resolves the package,
2. verifies `openvaf` or `openvaf-r` is on `PATH`,
3. compiles the package's Verilog-A sources into a cache directory,
4. writes a build metadata file:

```json
{
  "compiler": "openvaf-r",
  "compiler_version": "23.5.0",
  "osdi_abi": "0.5",
  "source_hash": "sha256:...",
  "built_at": "..."
}
```

Recommended cache location:

```text
$XDG_CACHE_HOME/ice/pdk/<package>-<version>/osdi/
```

or, if unavailable:

```text
~/.ice/cache/pdk/<package>-<version>/osdi/
```

Do not write into the package directory because that changes content hashes.

### 7.2 Compiler detection

The IHP PDK already detects `openvaf-r` first, then `openvaf`. Reuse that
behavior. On this machine:

```text
/usr/bin/openvaf-r
openvaf 23.5.0
```

The version string shown by OpenVAF Reloaded may still say `openvaf 23.5.0`;
this is expected.

### 7.3 Missing OSDI remediation

If OSDI is missing:

```text
simulation_ready: false
missing: ["OSDI models not built for this OpenVAF/OSDI ABI"]
hint: "Run ice-pkg pdk-build ihp.sg13g2"
```

Do not silently fall back to behavioral models.

## 8. Package contents and sizes

### 8.1 What to include

Include only the simulation subset in the package:

| PDK | Required content | Approx. size |
|---|---|---:|
| Sky130 | `libs.tech/ngspice` + `libs.ref/sky130_fd_pr/spice` | ~50 MB |
| GF180 | `libs.tech/ngspice/design.ngspice` + `sm141064*.ngspice` | ~1–2 MB |
| IHP | `libs.tech/ngspice/models` + `pdk/verilog-a` | ~1.2 MB |

Do not put the full 1–9 GB PDK into a normal Git package.

For full layout/DRC/LVS/PCell support, keep one of:

- a larger ICE PDK package containing the full data,
- a release archive downloaded by `ice-pkg` with SHA-256 verification,
- a Git LFS package,
- a local cache managed by `ice-pkg`.

Do not point at a Volare/CIEL installation as the full PDK source.

### 8.2 Sky130 include closure

Sky130's `libs.tech/ngspice` files include paths under
`../../../libs.ref/sky130_fd_pr/spice`. The package must preserve the relative
directory structure or rewrite the includes to package-relative paths.

### 8.3 Licensing

IHP, Sky130, and GF180 are Apache-2.0. Packages must preserve upstream
`LICENSE`, `NOTICE`, and attribution files. Do not vendor proprietary PDKs
without explicit permission.

## 9. Migration plan

### Phase 0: schema and resolver

- Extend `PackagePdk` and `package.toml` parsing.
- Add package-root discovery to `icelib`.
- Add canonical PDK ID/alias matching.
- Add `source`, `package`, `package_version`, `model_hash` to `pdk_info`.
- Remove Volare/CIEL/system-root discovery from the runtime PDK resolver.
- Migrate project `pdk` strings and `packageDependencies` once, then require the
  canonical package PDK IDs.
- Migrate the installed package manifests once to declare `[pdk].id` and
  `[pdk.simulation]`.

### Phase 1: IHP package

- Add `libs.tech/ngspice/models` and `pdk/verilog-a` to the `ihp.sg13g2` package.
- Add `[pdk.simulation]` metadata.
- Add `ice-pkg pdk-build` and OpenVAF detection.
- Verify with:

```sh
ice-cli pdk-info ihp-sg13g2 --probe
```

- Confirm ICE's libngspice path loads the OSDI models, not only the CLI.

### Phase 2: GF180 package

- Add `design.ngspice`, `sm141064.ngspice`, and required includes.
- Add `[pdk.simulation]` metadata.
- Verify `typical` section and `nfet_03v3`/`pfet_03v3`.

### Phase 3: Sky130 package

- Add `libs.tech/ngspice` plus the `sky130_fd_pr/spice` subset.
- Preserve relative include paths.
- Add `.option scale=1u` and example devices.
- Verify `tt` section.

### Phase 4: remove global init dependency

- Add per-simulation `osdi`/`initCommands` to libngspice.
- Make `.spiceinit` unnecessary for ICE simulations.
- Delete `PDK_ROOT`/`PDK` and global-`.spiceinit` handling from the simulation
  and PDK paths.
- Treat any remaining environment-based setup as a bug, not a fallback.

### Phase 5: package index and docs

- Update the ICE package index repositories.
- Document `ice-pkg pdk-build`.
- Update `docs/packages.md`, `docs/ice-mcp.md`, `docs/cli.md`,
  `docs/getting-started.md`, and generated `AGENTS.md`.
- Add `pdk` package examples.

### Phase 6: optional full-PDK support

- Add an archive/tarball download path with SHA-256 verification.
- Add an optional package-managed full-PDK payload for GDS/DRC/LVS/PCell data.
- Keep simulation independent of that optional payload.
- Do not add a Volare/CIEL path.

## 10. Acceptance criteria

A machine with only ICE and the ICE PDK package installed must pass. Volare,
CIEL, `PDK_ROOT`, and `PDK` must not be present or set:

```sh
ice-cli pdk-info ihp-sg13g2 --probe
ice-cli pdk-info sky130A --probe
ice-cli pdk-info gf180mcuA --probe
```

Each must report:

```json
{
  "source": "ice-package",
  "simulation_ready": true,
  "probe": {"ok": true}
}
```

A transistor-level ring oscillator must run from an ICE project using each
package. `ice_doctor` must show the package source, model hash, OSDI/build
status, and no missing external paths.

Existing project files are migrated once to the package PDK IDs. After the
migration, the corresponding ICE package is required; Volare/CIEL are not used
even when installed.

## 11. Risks and tradeoffs

- **Package size:** Sky130's simulation subset is ~50 MB; use Git LFS or an
  archive if the package repository should stay small.
- **OSDI ABI:** `.osdi` files must match the ngspice/OSDI ABI. Build locally and
  record compiler/ABI versions.
- **Trust:** package scripts and Verilog-A compilation execute code. Keep
  `pdk-build` explicit and never run it implicitly during install.
- **Duplication:** users may still have Volare/CIEL installed, but ICE will
  ignore those copies. Document that the ICE package is the only simulation
  source, so users do not edit the wrong model tree.
- **Updates:** package releases must be versioned and lockfile-pinned.
- **Full PDK expectations:** simulation independence does not automatically
  provide GDS, PCells, DRC, or LVS; document the split.

## 12. Open questions

1. Should the canonical PDK ID live in the package (`[pdk].id`) or be derived
   from the package name? Recommended: explicit `[pdk].id`.
2. Should Sky130 package one canonical variant (`sky130A`) or both A/B?
3. Should OSDI build output be cached per package version, per OSDI ABI, or
   both? Recommended: both.
4. Should `ice-pkg install` support release archives in phase 1, or only Git
   packages?
5. Should the simulation subset live in the same package repo as symbols/layout
   or in a separate `pdk-sim` package that the main package depends on?
6. Should `pdk_info --probe` run both CLI and libngspice probes by default?

## 13. Files and APIs likely to change

```text
include/icelib/packages.hpp
src/icelib/packages.cpp

include/icelib/pdk.hpp
src/icelib/pdk.cpp

include/icelib/simulation.hpp
src/icelib/simulation.cpp

src/ice-mcp/main.cpp
src/ice-cli/main.cpp
src/ice-pkg/main.cpp

src/ice/package_ui.cpp
src/ice/package_ui.hpp
src/ice/layout_devices.cpp

tests/core_tests.cpp
tests/mcp_tools_smoke.py
tests/icepy_smoke.py

docs/packages.md
docs/ice-mcp.md
docs/cli.md
docs/simulation.md
docs/getting-started.md
```

## 14. Implementation status (2026-11-01)

Implemented:

- `[pdk].id` / aliases and `[pdk.simulation]` parsing.
- Package-only PDK discovery in `icelib`.
- `pdk_info`, `ice_doctor`, and `pdk_build` in CLI and MCP.
- Package-local ngspice model trees for IHP, Sky130, and GF180.
- IHP Verilog-A sources and local OSDI cache build.
- Explicit libngspice OSDI initialization; no `~/.spiceinit` dependency.
- Package setup injection for CLI, MCP, GUI, and schematic preview.
- One-time `project_migrate` normalization of project PDK IDs.
- Hard cutover: no Volare/CIEL/`PDK_ROOT` fallback.
- Canonical PDK IDs in GUI creation dialogs and package lookup by ID/alias.
- `pdk_id` plus package SHA-256 content hash in `ice.lock`.
- Documentation builds cleanly with the declared Sphinx/Myst/Furo extensions.
- CI now builds the documentation with the declared requirements.
- Package-local layout device libraries (`layout/devices.json`) resolved through
  the active PDK package instead of a bundled IHP-only asset.
- Package-local PCell generators for all three PDKs:
  - IHP keeps the complete device library and evaluates vendored PyCell sources
    under `upstream/ihp`.
  - Sky130 and GF180MCU ship ICE-native PCell replicas implemented with the
    `icepy` layout builder API.
- Deterministic library regeneration scripts (`layout/generate_devices.py`)
  for the ICE-native packages.
- Package PCell regression test covering every declared generator.

Still pending for later phases:

- Publishing the updated package repositories and the `ice-packages` submodule
  pointers with valid GitHub credentials.
- Optional package-managed full-PDK payload for GDS/DRC/LVS data beyond the
  device PCell libraries.
- Full package-manager integration tests in CI. CI now builds docs; it does not
  yet install PDK packages or run package-based simulations.

## 15. Next steps

The hard-cutover core and package-local PCell libraries are implemented and
verified. Remaining work:

1. Commit and publish the updated `ice-pkg-ihp-sg13g2`, `ice-pkg-sky130`, and
   `ice-pkg-gf180mcu` repositories, then update the `ice-packages` submodules.
2. Add CI coverage that installs only ICE packages and runs one package-based
   simulation for each PDK.
3. Add CI coverage that installs the three packages and runs
   `tests/package_pcells_test.py`.
4. Extend the ICE-native PCell replicas with process-specific DRC-clean
   geometry as the packages gain full layout payloads.
