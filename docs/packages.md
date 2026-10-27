# ICE Packages

Packages are directories containing a versioned `package.toml` manifest and
capability-oriented content such as `pdk/`, `symbols/`, `models/`, `layout/`,
`scripts/`, and `docs/`.

`ice-pkg resolve <repository> <ice-version> name=constraint ...` selects the
highest matching local package version, resolves transitive dependencies, and
writes a deterministic `ice.lock`. The lockfile records exact versions,
sources, trust declarations, capabilities, and SHA-256 content hashes.

Package code is untrusted by default. A package's `[trust] trusted = true`
metadata is recorded for policy decisions but does not grant execution
permission by itself. Scripts and plugins must remain behind an explicit
permission boundary.

The example packages under `examples/packages` demonstrate a minimal PDK,
library dependency, symbol metadata, and SVG artwork. The default index ships
separate packages for:

- `ihp.sg13g2` — IHP SG13G2 130 nm BiCMOS
- `sky130` — SkyWater SKY130 130 nm CMOS
- `gf180mcu` — GlobalFoundries GF180MCU 180 nm CMOS

External Git packages can be installed into a local repository with:

```text
ice-pkg install <repository> <name> <version> <git-url> [revision]
```

The installer clones the source and writes an ICE wrapper manifest when the
repository does not already provide one. It does not execute package scripts.
Each standalone PDK package points to its upstream Apache-2.0 repository; the
upstream installation guides and preview status still apply.

## PDK metadata

A package that provides a PDK can declare its display name, default schematic,
and layout layer stack in `package.toml`:

```toml
[pdk]
display_name = "SkyWater SKY130"
default_schematic = "sky130_top"

[[pdk.layer]]
name = "nwell"
number = 64

[[pdk.layer]]
name = "diff"
number = 65
```

Activating the package adds these layers to the Layout editor, records the
package as a project dependency, and creates the default schematic when the
project has no schematics yet. Layer GDS numbers are currently stored as single
integers; non-zero GDS datatypes are not represented by the layout model.

## Package indexes

The GUI reads available packages from a Git package index. The default index is
https://github.com/ldapx/ice-packages, cloned into `~/.ice/package-index`.
Opening **Manage Packages** automatically clones the default index when it is
missing or empty, so the repository's packages are shown without pressing
**Refresh**. **Refresh** still performs an explicit pull and submodule update.

Indexed packages are Git submodules. Each package directory contains a
`package.toml` manifest and may provide `.github/logo.png`, which the package
manager displays as the package logo. The submodule URL and branch recorded in
`.gitmodules` are used when installing the package.

## GUI package manager

Open `Project -> Manage Packages` to:

- browse every package in the index with its description, version, capabilities,
  and `.github/logo.png` logo;
- install an indexed package into `~/.ice/packages`;
- install a package from an arbitrary repository URL with **Install from
  Repository URL…**;
- uninstall any installed package, including packages that are not currently in
  the index;
- refresh the index and initialize its submodules;
- activate any installed PDK package when a project is open.

Installing and uninstalling packages work without an open project. Installing a
package writes an `ice.lock` beside the project when a project is open. A
package that was removed from the index but is still installed remains visible
so it can be uninstalled.

## Project package dependencies

Every project records its package dependencies. Opening a project verifies that
each dependency is installed. If packages are missing, ICE lists them and offers
to install them before the project opens; closing the prompt leaves the project
closed.

Creating a schematic asks for its PDK. The built-in **Generic PDK** is always
available. Installed packages that provide the `pdk` capability appear as
additional choices. Selecting a package PDK adds that package to the project
dependency list after confirmation; declining the prompt cancels schematic
creation. The schematic stores its PDK in the project file.

A schematic using the Generic PDK browses only the built-in generic symbols. A
schematic using a package PDK browses the built-in generic symbols plus that
package's `symbols/` library. Activating a PDK package adds its declared
technology layers to the Layout editor and records the package as a project
dependency.

## PDK-provided procedural layout operations

Procedural layout scripts can request process-specific geometry through the
generic layout builder API. ICE dispatches these requests to the active PDK
package after the user script finishes:

```python
layout.add_dummy_fill(layers=["Metal1", "Metal2"], density=50, deviation=10)
```

For dummy fill, the package contract is `layout/dummy_fill.py` with an
`add_dummy_fill(layout, **options)` function. The implementation may inspect
the builder through `layout.iter_polygons()`, `layout.bounds()`, and
`layout.layer_number()`, and it adds geometry through the normal
`layout.layer(...).rectangle()` / `.polygon()` API. Calling
`layout.add_dummy_fill(...)` is an explicit request to execute package code;
PDKs without the implementation report a clear error. The IHP SG13G2 package
implements dummy metal fill for its metal stack using rules adapted from
`gdsfill`.

The File menu also provides **Open GDS/OASIS Layout** and **Save GDS/OASIS
Layout**. These actions use native GDSII (`*.gds`, `*.gdsii`) for the documented
deterministic BOUNDARY-only subset. OASIS extensions are accepted for workflow
compatibility but OASIS import/export is explicitly rejected until a native
OASIS adapter exists.

## PDK-provided device generators

Layout device libraries declare PCell generator metadata (`module` and `class`).
ICE loads the generator from the active PDK package:

```text
<pdk-root>/layout/device_generator.py
```

The IHP SG13G2 package supplies this hook and routes generation through
`icepy.pcells`, which evaluates the upstream IHP PyCell implementation and
transfers polygons into an `icepy` layout cell. Building `icepy` is required for
PDK device regeneration.
