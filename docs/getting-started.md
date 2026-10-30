# Getting Started

This page covers prerequisites, building, and testing ICE.
Product intent is in {doc}`vision`; technical contracts are in {doc}`architecture`.

## Components

| Target | Description |
| --- | --- |
| `icelib` | Shared C++ core library (domain model, operations, adapters). |
| `ice` | Main Qt 6 desktop EDA application (schematic, layout, simulation, analysis). |
| `ice-cli` | Headless automation CLI over the same `icelib` operations. |
| `ice-pkg` | Package and PDK installation, resolution, verification, and lockfile management. |
| `ice-mcp` | MCP server exposing approved `icelib` operations to external agents. |
| `icepy` | Optional Python bindings (`nanobind`) for project automation, layout building, and PDK PCell generation. |

Key integrations (behind adapter boundaries): Qt 6, Clipper2 geometry,
ngspice (`libngspice`), ALIGN, GDSII/OASIS interchange, TOML packages,
Python scientific stack.

## Prerequisites

- C++20 compiler
- [xmake](https://xmake.io/) 2.9.8+ (build system, see `xmake.lua`)
- Qt 6 (Widgets, Gui, Core, Svg, SvgWidgets) for the `ice` target
- `spdlog`, `nlohmann_json` (via xmake packages)
- `libngspice`, `toml++`, `libcrypto` headers (system layout, see `xmake.lua`)
- Python 3.14 + `nanobind` headers (only for the optional `icepy` target)
- `clang-format` (formatting via `xmake format`)
- The bundled `agentsdk-cpp` submodule. Clone with `--recursive`, or run
  `git submodule update --init --recursive` after cloning. The build produces
  `libagentsdk.so` and installs it alongside ICE

## Build

```sh
xmake build doctest ice-cli ice-pkg ice-mcp
```

Build the desktop app (requires Qt 6):

```sh
xmake build ice
```

Build the optional Python bindings:

```sh
xmake build icepy
export PYTHONPATH="$PWD/build/linux/x86_64/release:$PWD/python"
python3 -c "import icepy; print(icepy.new_project('demo').name)"
```

See {doc}`icepy` for the layout builder and PDK PCell runtime.

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

## Build the docs

Install the docs dependencies and build this site:

```sh
pip install -r docs/requirements.txt
sphinx-build -b html docs docs/_build
```

or via xmake:

```sh
xmake docs
```

## Next steps

- {doc}`vision` — what ICE is and why it exists
- {doc}`gui` — desktop shell and editors
- {doc}`simulation` — simulation contract and testbenches
- {doc}`packages` — packages, PDK installation, and GDSII/OASIS interchange
