# OpenVLSI

<div class="openvlsi-hero">
<img class="openvlsi-logo" src="_static/logo.png" alt="OpenVLSI logo" />
<div class="openvlsi-hero-name">OpenVLSI</div>
<div class="openvlsi-hero-text">A programmable integrated-circuit design environment</div>
<div class="openvlsi-hero-tagline">Schematic, layout, simulation, and AI-assisted automation on one canonical core — <code>openvlsilib</code>.</div>
<div class="openvlsi-hero-actions">
<a class="openvlsi-btn-brand" href="user/index.html">User Guide →</a>
<a class="openvlsi-btn-alt" href="developer/index.html">Developer Guide</a>
<a class="openvlsi-btn-alt" href="https://github.com/openvlsi">GitHub</a>
</div>
</div>

<div class="openvlsi-features">
<div class="openvlsi-feature"><div class="openvlsi-feature-title">🧩 Core-first</div>The GUI, CLI, Python API, MCP server, and agent chatbox are interfaces to the same validated <code>openvlsilib</code> operations — not separate implementations.</div>
<div class="openvlsi-feature"><div class="openvlsi-feature-title">🎯 Deterministic</div>Integer database units, explicit schema versions, and reproducible lockfiles keep geometry, simulation, and packages reproducible.</div>
<div class="openvlsi-feature"><div class="openvlsi-feature-title">🤖 Programmable</div>Manual editing, Python scripting, package installation, verification, and AI agents operate on the same layout database.</div>
</div>

## Documentation

The documentation is split by audience so each page has one job:

- **{doc}`/user/index`** — create projects, draw schematics and layouts, run
  simulations, use the CLI, install PDKs, and automate with Python or agents.
- **{doc}`/developer/index`** — build and test OpenVLSI, understand the architecture,
  extend the package/plugin system, and contribute to the project.
- **{doc}`/project/index`** — product vision and the implementation roadmap.

```{toctree}
:maxdepth: 1
:caption: User Guide

user/index
```

```{toctree}
:maxdepth: 1
:caption: Developer Guide

developer/index
```

```{toctree}
:maxdepth: 1
:caption: Project

project/index
```
