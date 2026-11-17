# Building and Testing

This page is for developers building ICE from source. If you only want to use
ICE, start with the {doc}`/user/getting-started` guide instead.

Product intent is in {doc}`/project/vision`; technical contracts are in
{doc}`architecture`. The workspace layout and contribution workflow are in
{doc}`plan`.

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
- `spdlog`, `nlohmann_json`, `clipper2` (via xmake packages)
- `libngspice`, `toml++`, `libcrypto` headers (system layout, see `xmake.lua`)
- Python 3.14 + `nanobind` headers (only for the optional `icepy` target)
- `clang-format` (formatting via `xmake format`)
- The bundled `agentsdk-cpp` submodule. Clone with `--recursive`, or run
  `git submodule update --init --recursive` after cloning. The build produces
  `libagentsdk.so` and installs it alongside ICE.

## Build

```sh
xmake build doctest ice-cli ice-pkg ice-mcp
```

For a reproducible build fingerprint, configure with the git revision and an
explicit timestamp:

```sh
ICE_BUILD_COMMIT="$(git rev-parse --short HEAD)" \
ICE_BUILD_TIME="$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
xmake f -m release -y
```

After installing a new build, restart any long-lived `ice-mcp` process. The
server keeps the code it loaded at process start. `ice-cli --version` and the
MCP `ice_version` tool print the commit, build time, executable path, and
feature flags.

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

See {doc}`/user/icepy` for the layout builder and PDK PCell runtime.

Simulation PDKs are provided by ICE packages under `~/.ice/packages` (or
`ICE_PACKAGE_ROOT`); Volare, CIEL, `PDK_ROOT`, and `PDK` are not used.
OSDI-based packages need a one-time local build:

```sh
ice-cli pdk-build ihp-sg13g2
```

Verify the package before simulating with:

```sh
ice-cli pdk-info ihp-sg13g2 --probe
```

See {doc}`/user/packages` for installing and selecting packages, and
{doc}`packages` for the package format and PDK authoring contracts.

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

Pure-Python test suites:

```sh
PYTHONPATH=python python3 tests/icepy_sim_tests.py
python3 tests/icepy_report_runner_tests.py
python3 tests/procedural_router_test.py
```

## Build the documentation

Install the docs dependencies and build the site from the repository root:

```sh
python3 -m pip install -r docs/requirements.txt
sphinx-build -b html docs docs/_build -W --keep-going
```

or via xmake:

```sh
xmake docs
```

The API reference for procedural layouts is generated with Sphinx autodoc from
`assets/layout/procedural_runner.py`. See {doc}`/user/procedural-layout` for the
published page and {doc}`plan` for the documentation structure.

## Next steps

- {doc}`architecture` — domain model, interfaces, and subsystem contracts.
- {doc}`capabilities` — GUI / CLI / Python capability parity.
- {doc}`packages` — package format, device libraries, and PDK integration.
- {doc}`plan` — how the documentation is organized.
