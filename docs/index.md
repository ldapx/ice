# ICE

<div class="ice-hero">
<img class="ice-logo" src="_static/logo.png" alt="ICE logo" />
<div class="ice-hero-name">ICE</div>
<div class="ice-hero-text">A programmable integrated-circuit design environment</div>
<div class="ice-hero-tagline">Schematic, layout, simulation, and AI-assisted automation on one canonical core — <code>icelib</code>.</div>
<div class="ice-hero-actions">
<a class="ice-btn-brand" href="user/index.html">User Guide →</a>
<a class="ice-btn-alt" href="developer/index.html">Developer Guide</a>
<a class="ice-btn-alt" href="https://github.com/ldapx">GitHub</a>
</div>
</div>

<div class="ice-features">
<div class="ice-feature"><div class="ice-feature-title">🧩 Core-first</div>The GUI, CLI, Python API, MCP server, and agent chatbox are interfaces to the same validated <code>icelib</code> operations — not separate implementations.</div>
<div class="ice-feature"><div class="ice-feature-title">🎯 Deterministic</div>Integer database units, explicit schema versions, and reproducible lockfiles keep geometry, simulation, and packages reproducible.</div>
<div class="ice-feature"><div class="ice-feature-title">🤖 Programmable</div>Manual editing, Python scripting, package installation, verification, and AI agents operate on the same layout database.</div>
</div>

## Documentation

The documentation is split by audience so each page has one job:

- **{doc}`/user/index`** — create projects, draw schematics and layouts, run
  simulations, use the CLI, install PDKs, and automate with Python or agents.
- **{doc}`/developer/index`** — build and test ICE, understand the architecture,
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
