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
library dependency, symbol metadata, and SVG artwork. The IHP SG13G2 package is
maintained separately at https://github.com/ldapx/ice-pkg-ihp on the `dev`
branch.

External Git packages can be installed into a local repository with:

```text
ice-pkg install <repository> <name> <version> <git-url> [revision]
```

The installer clones the source and writes an ICE wrapper manifest when the
repository does not already provide one. It does not execute package scripts.
The standalone IHP package definition provides the IHP SG13G2 PDK metadata and
points to the upstream Apache-2.0 repository; the upstream installation guide
and preview status still apply.

## Package indexes

The GUI reads available packages from a Git package index. The default index is
https://github.com/ldapx/ice-packages. During local development ICE prefers an
existing `~/repositories/ice-packages` checkout; otherwise it uses
`~/.ice/package-index` and clones the default index when **Refresh** is used.

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
- activate the installed IHP SG13G2 technology when a project is open.

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
package's `symbols/` library. Activating IHP SG13G2 adds the technology layers to
the Layout editor and records the package as a project dependency.

The File menu also provides **Open GDS/OASIS Layout** and **Save GDS/OASIS
Layout**. These actions use native GDSII (`*.gds`, `*.gdsii`) for the documented
deterministic BOUNDARY-only subset. OASIS extensions are accepted for workflow
compatibility but OASIS import/export is explicitly rejected until a native
OASIS adapter exists.
