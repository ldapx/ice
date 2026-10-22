
# <img src=".github/logo.png" width="64"> <sup>**ICE**</sup>
<sup><sub>
**IC E**DA - **I**ntegrated **C**ircuits **E**lectronics Design Automation
</sub></sup>

![screenshot](.github/screenshot_1.png)

ICE is an open-source, extensible integrated-circuit design environment for
schematic capture, IC layout, analog simulation, analysis, automated layout,
and programmable design workflows.

One canonical design model and one set of core operations live in `icelib`.
The executables and bindings below are interfaces to that core, not separate
implementations.

## Components

| Target | Description |
| --- | --- |
| `icelib` | Shared C++ core library (domain model, operations, adapters). |
| `ice` | Main Qt 6 desktop EDA application (schematic, layout, simulation, analysis). |
| `ice-cli` | Headless automation CLI over the same `icelib` operations. |
| `ice-pkg` | Package and PDK installation, resolution, verification, and lockfile management. |
| `ice-mcp` | MCP server exposing approved `icelib` operations to external agents. |
| `icepy` | Optional Python bindings (`nanobind`) for project automation. |

Key integrations (behind adapter boundaries): Qt 6, Clipper2 geometry,
ngspice (`libngspice`), ALIGN, GDSII/OASIS interchange, TOML packages,
Python scientific stack.

## Principles

- Core-first: the GUI is a client of `icelib`, not the primary abstraction.
- Manual, scripted, and AI-assisted edits operate on the same layout database.
- External tools are accessed through versioned adapters; their types do not leak into the core model.
- Results are deterministic and reproducible from declared inputs and recorded versions.

## Prerequisites

- C++20 compiler
- [xmake](https://xmake.io/) 2.9.8+ (build system, see `xmake.lua`)
- Qt 6 (Widgets, Gui, Core, Svg, SvgWidgets) for the `ice` target
- `spdlog`, `nlohmann_json` (via xmake packages)
- `libngspice`, `toml++`, `libcrypto` headers (system layout, see `xmake.lua`)
- Python 3.14 + `nanobind` headers (only for the optional `icepy` target)
- `clang-format` (formatting via `xmake format`), `doctest` sources (bundled under `include/doctest`)

## Build

```sh
xmake build doctest ice-cli ice-pkg ice-mcp
```

Build the desktop app (requires Qt 6):

```sh
xmake build ice
```

## Test

```sh
xmake run doctest
```

or:

```sh
xmake test
```

Format first-party C++ sources:

```sh
xmake format
```

## Documentation

Full documentation is in [`docs/`](docs/) (Sphinx, Furo theme with an
xmake-docs-inspired palette). Build it with:

```sh
pip install -r docs/requirements.txt
sphinx-build -b html docs docs/_build
```

or:

```sh
xmake docs
```

| Page | Contents |
| --- | --- |
| Getting Started | Prerequisites, build, test, and docs build. |
| Vision | Product purpose, principles, long-term direction. |
| Architecture | Domain model, interfaces, adapters, subsystem contracts. |
| GUI | Desktop shell, schematic/layout editors, interaction contracts. |
| Roadmap | Capabilities and acceptance criteria. |
| Capabilities | GUI / CLI / Python capability parity matrix. |
| Packages, Simulation, Symbols, Plugins | Subsystem contracts. |
| Documentation Guide | How the docs pages relate. |

## Project status

Early development. File formats, plugin APIs, and package schemas may break
between versions without migration paths; version stamps are recorded so
breakage is detectable. See the roadmap in `docs/` for the current scope.

## License

MIT. See [LICENSE](LICENSE).
