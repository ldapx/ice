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
library dependency, symbol metadata, and SVG artwork. The IHP SG13G2 package
definition is maintained separately at
`~/repositories/ice-pkg-ihp-sg13g2`.

External Git packages can be installed into a local repository with:

```text
ice-pkg install <repository> <name> <version> <git-url> [revision]
```

The installer clones the source and writes an ICE wrapper manifest. It does
not execute package scripts. The standalone IHP package definition provides the IHP SG13G2 PDK metadata and
points to the upstream Apache-2.0 repository; the upstream installation guide
and preview status still apply.

In the GUI, open `Project -> Manage Packages`, keep the default IHP package URL,
and choose **Install and activate IHP SG13G2**. ICE installs the package into
`~/.ice/packages`, writes an `ice.lock` beside the project when the project has
been saved, adds the SG13G2 layers to the Layout editor, and exposes IHP
primitive devices in `Project -> Place Symbol`. The technology layers are
stored in the project, so reopening the project restores the IHP symbol set.

The File menu also provides **Open GDS/OASIS Layout** and **Save GDS/OASIS
Layout**. These actions use native GDSII (`*.gds`, `*.gdsii`) for the documented
deterministic BOUNDARY-only subset. OASIS extensions are accepted for workflow
compatibility but OASIS import/export is explicitly rejected until a native
OASIS adapter exists.
