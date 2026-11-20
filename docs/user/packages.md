# Installing and Managing Packages

ICE packages are versioned directories that provide PDK data, symbols, device
libraries, simulation models, technology rules, and implementation assets. A
project records the packages it depends on, so designs remain reproducible.

This page covers installing, resolving, and selecting packages. Package
structure, metadata, device generators, and PDK authoring are documented in the
{doc}`/developer/packages` guide.

## Bundled packages

The default package index ships:

- `ihp.sg13g2` — IHP SG13G2 130 nm BiCMOS
- `sky130` — SkyWater SKY130 130 nm CMOS
- `gf180mcu` — GlobalFoundries GF180MCU 180 nm CMOS

The example packages under `examples/packages` demonstrate a minimal PDK,
library dependency, symbol metadata, and SVG artwork.

## Install and resolve packages

`ice-pkg resolve` selects the highest matching local package version, resolves
transitive dependencies, and writes a deterministic `ice.lock`:

```sh
ice-pkg resolve <repository> <ice-version> name=constraint ...
```

Install an external Git package into a local repository with:

```sh
ice-pkg install <repository> <name> <version> <git-url> [revision]
```

The installer clones the source and writes an ICE wrapper manifest when the
repository does not already provide one. It does not execute package scripts.
The lockfile records exact versions, sources, trust declarations, capabilities,
canonical `pdk_id`, and SHA-256 content hashes.

Package code is untrusted by default. Procedural layouts refuse to execute a
PDK package's device generator unless the package declares `[trust] trusted =
true`, or the caller explicitly sets `ICE_ALLOW_PACKAGE_CODE=1` for that
invocation. The trust flag is advisory metadata, not a signature; use the
environment override only for packages you have reviewed.

## Build and verify a PDK

Simulation PDKs come from ICE packages only. Volare, CIEL, `PDK_ROOT`, `PDK`,
and system PDK roots are not runtime fallbacks. Packages that use Verilog-A
models need a one-time OSDI build:

```sh
ice-cli pdk-build ihp-sg13g2
```

Verify the installation before simulating:

```sh
ice-cli doctor [project] --probe
ice-cli pdk-info <pdk> --probe
```

`doctor` reports Python, ngspice, installed PDKs, simulation model files, and
Liberty timing libraries. `pdk-info` shows the exact model directory, section
names, OSDI/OpenVAF status, recommended setup lines, example device names, and
a minimal transistor load check.

## Use the GUI package manager

Open **Project → Manage Packages** to:

- browse every indexed package with its description, version, capabilities,
  and `.github/logo.png` logo;
- install an indexed package into `~/.ice/packages`;
- install a package from an arbitrary repository URL with **Install from
  Repository URL…**;
- uninstall any installed package, including packages no longer in the index;
- refresh the index and initialize its submodules;
- activate any installed PDK package when a project is open.

Installing and uninstalling work without an open project. Installing a package
writes an `ice.lock` beside the project when a project is open. A package that
was removed from the index but is still installed remains visible so it can be
uninstalled.

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

## Next steps

- {doc}`/developer/packages` — package format, metadata, device libraries, and
  PDK connectivity/authoring contracts.
- {doc}`cli` — `doctor`, `pdk-info`, and `pdk-build` command details.
- {doc}`gui` — package manager and PDK activation in the desktop app.
- {doc}`simulation` — simulation setup and model loading.
