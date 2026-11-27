# Documentation Guide

OpenVLSI documentation is split by audience. User pages explain how to accomplish
tasks with the shipped interfaces. Developer pages describe contracts, build
workflow, extension points, and project direction. This separation keeps
task-oriented guidance short without hiding the engineering details.

## Site structure

```text
docs/
├── index.md                     # landing page and audience chooser
├── user/                        # using OpenVLSI
│   ├── index.md
│   ├── getting-started.md
│   ├── gui.md
│   ├── cli.md
│   ├── simulation.md
│   ├── symbols.md
│   ├── packages.md              # install, resolve, and select packages
│   ├── openvlsipy.md
│   ├── procedural-layout.md
│   ├── openvlsi-mcp.md
│   └── agent-cookbook.md
├── developer/                   # building and extending OpenVLSI
│   ├── index.md
│   ├── building.md
│   ├── architecture.md
│   ├── capabilities.md
│   ├── packages.md              # package/PDK authoring contracts
│   ├── plugins.md
│   ├── plan.md
│   └── proposals/               # unpublished design proposals
├── project/                     # direction and delivery
│   ├── index.md
│   ├── vision.md
│   └── roadmap.md
├── conf.py
├── requirements.txt
└── _static/
```

## User Guide

Use the User Guide for anything a person does with a released or locally built
OpenVLSI:

- install or build prerequisites, launch the app, and create a project;
- use the desktop workspaces, CLI commands, simulation flow, and package
  manager;
- write Python automation, procedural layout scripts, and MCP workflows;
- look up symbols and command syntax.

User pages should lead with the task and a working example, then link to
developer pages for internals. They should not document private C++ classes or
package implementation contracts.

## Developer Guide

Use the Developer Guide for anything a contributor or integrator needs:

- build, test, and documentation workflow;
- architecture, domain contracts, process boundaries, and subsystem design;
- package/PDK authoring, device generators, connectivity, and extraction;
- plugin lifecycle and permission contracts;
- capability parity and contribution conventions.

Developer pages may link to user pages for workflows, but should not duplicate
task instructions. Package installation belongs in {doc}`/user/packages`;
package format and authoring belong in {doc}`packages`.

## Project

The Project pages record product intent and delivery scope. {doc}`/project/vision`
explains why OpenVLSI exists and what it should become. {doc}`/project/roadmap`
records capabilities, dependencies, and acceptance criteria.

When pages disagree:

1. Preserve the core principles in {doc}`/project/vision`.
2. Update the contracts and boundaries in {doc}`architecture`.
3. Reflect the resulting scope in {doc}`/project/roadmap`.

The roadmap is intentionally narrower than the long-term vision. Features not
listed there are proposals, not current implementation commitments.

## Cross-references

- Within the same directory, use a short reference such as `{doc}`gui``.
- Across areas, use an absolute reference such as `{doc}`/user/packages`` or
  `{doc}`/project/vision``.
- When moving a page, update every `{doc}` reference. Sphinx `-W` builds fail
  on broken or orphaned documents.
- Add new pages to the nearest `index.md` toctree, not directly to the site
  root.

## Adding or moving pages

1. Decide whether the page is task-oriented (User Guide), contract-oriented
   (Developer Guide), or direction-oriented (Project).
2. Add the file under the matching directory and update the area `index.md`.
3. Update cross-references and the README documentation table if the page is a
   major entry point.
4. Build with warnings as errors:

   ```sh
   python3 -m pip install -r docs/requirements.txt
   sphinx-build -b html docs docs/_build -W --keep-going
   ```

5. Check that non-published proposals are listed in `exclude_patterns` rather
   than left as orphan documents.

## API references

The procedural layout API reference is generated with `sphinx.ext.autodoc`
from `assets/layout/procedural_runner.py`. Docstrings in that module are the
source of truth for the published builder reference. See
{doc}`/user/procedural-layout` for the rendered page.

Python bindings (`openvlsipy`) are an optional compiled extension, so their prose
stays in {doc}`/user/openvlsipy`; do not make the documentation build depend on the
extension being built.
