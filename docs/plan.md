# Documentation Guide

ICE documentation is split so that product intent, technical contracts, and
day-to-day usage can evolve independently. This site is the canonical
documentation; it replaces the former loose Markdown files at the repository
root.

## Pages

- {doc}`vision` — product purpose, principles, scope, and long-term direction.
- {doc}`architecture` — domain model, interfaces, technology boundaries,
  integrations, and subsystem requirements.
- {doc}`gui` — desktop shell, schematic editor, layout editor, and
  interaction contracts.
- {doc}`roadmap` — capabilities, dependencies, and acceptance criteria.
- {doc}`capabilities` — GUI / CLI / Python capability parity matrix.
- {doc}`packages` — packages, PDK installation, and GDSII/OASIS interchange.
- {doc}`plugins` — experimental plugin lifecycle and permission contract.
- {doc}`simulation` — simulation contract and testbench model.
- {doc}`symbols` — built-in schematic symbol library.
- {doc}`getting-started` — prerequisites, build, test, and docs build.

## How to use these pages

The vision explains **why** ICE exists and what it should become. The architecture
explains **what must be true** of the system and its boundaries. The roadmap
explains **the capabilities** and how to verify them.

When the pages disagree:

1. Preserve the core principles in {doc}`vision`.
2. Update the contracts and boundaries in {doc}`architecture`.
3. Reflect the resulting scope in {doc}`roadmap`.

The roadmap is intentionally narrower than the long-term vision. Features not
listed there are proposals, not current implementation commitments.
