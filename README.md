<h1 align="center">
<img alt="banner" src=".github/banner.png"/>
</h1>

<p align="center">
  <a href="https://openvlsi.org/docs/">
    <img alt="Documentation" src="https://img.shields.io/website?down_message=offline&label=docs&up_color=4d4d4d&up_message=online&url=https%3A%2F%2Fopenvlsi.org%2Fdocs"
  ></a>
  <a href="https://openvlsi.org/">
    <img alt="OpenVLSI website" src="https://img.shields.io/website?down_message=offline&label=openvlsi.org&up_color=ff002b&up_message=online&url=https%3A%2F%2Fopenvlsi.org"
  ></a>
  <a href="https://discord.gg/hvVThBAFQ3">
    <img alt="Discord Server" src="https://img.shields.io/discord/1575746428540682372"
  ></a>
  <a href="https://github.com/openvlsi/openvlsi/blob/main/LICENSE">
    <img alt="MIT License" src="https://img.shields.io/badge/license-MIT-brightgreen"
  ></a>
</p>

OpenVLSI is an open-source, extensible integrated-circuit design environment for
schematic capture, IC layout, analog simulation, analysis, automated layout,
and programmable design workflows.

![screenshot](.github/screenshot_1.png)

One canonical design model and one set of core operations live in `openvlsilib`.
The executables and bindings below are interfaces to that core, not separate
implementations.

## Components

| Target | Description |
| --- | --- |
| `openvlsilib` | Shared C++ core library (domain model, operations, adapters). |
| `openvlsi` | Main Qt 6 desktop EDA application (schematic, layout, simulation, analysis). |
| `openvlsi-cli` | Headless automation CLI over the same `openvlsilib` operations. |
| `openvlsi-pkg` | Package and PDK installation, resolution, verification, and lockfile management. |
| `openvlsi-mcp` | MCP server exposing approved `openvlsilib` operations to external agents. |
| `openvlsipy` | Optional Python bindings (`nanobind`) for project automation, layout building, and PDK PCell generation. |

Key integrations (behind adapter boundaries): Qt 6, Clipper2 geometry,
ngspice (`libngspice`), ALIGN, GDSII/OASIS interchange, TOML packages,
Python scientific stack.

## Principles

- Core-first: the GUI is a client of `openvlsilib`, not the primary abstraction.
- Manual, scripted, and AI-assisted edits operate on the same layout database.
- External tools are accessed through versioned adapters; their types do not leak into the core model.
- Results are deterministic and reproducible from declared inputs and recorded versions.

## Project layout

An OpenVLSI project is a directory:

```text
project/
├── project.ovpro        # project metadata
├── sch/                 # *.ovsch schematic cells
├── lay/                 # *.ovlay layout cells and procedural layouts
├── sim/                 # *.ovsim simulations
├── scripts/             # project-level Python simulation scripts
├── build/               # generated simulation runs and build output, ignored by Git
├── .gitignore
└── AGENTS.md
```

All project files are JSON except simulation scripts. `project.ovpro` contains
only project-level metadata, such as identity, name, units, grid, package
dependencies, layers, and simulation script associations. Schematics, layouts, and
simulations are stored as individual files.

## Prerequisites

OpenVLSI builds on Linux, macOS, and Windows. Dependencies are resolved through
`pkg-config`.

- C++20 compiler
- [xmake](https://xmake.io/) 3.1+ (build system, see `xmake.lua`)
- Qt 6 (Widgets, Gui, Core, Svg, SvgWidgets) for the `openvlsi` target
- `libngspice`, `toml++`, `spdlog`, `nlohmann_json`, OpenSSL, zlib
- Bundled `agentsdk-cpp` submodule for the Agent Client Protocol tab and MCP
  server. Clone with `--recursive`, or run
  `git submodule update --init --recursive` after cloning
- `clang-format` (formatting via `xmake format`), `doctest` sources (bundled
  under `include/doctest`)

Linux (Debian/Ubuntu):

```sh
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
    qt6-base-dev qt6-svg-dev libngspice0-dev libtomlplusplus-dev \
    libssl-dev zlib1g-dev libcurl4-openssl-dev
```

macOS:

```sh
brew install qtbase qtsvg libngspice tomlplusplus spdlog nlohmann-json \
    openssl@3 curl zlib simdjson pkgconf
export PKG_CONFIG_PATH="$(brew --prefix qtbase)/lib/pkgconfig:$(brew --prefix qtsvg)/lib/pkgconfig:$(brew --prefix)/lib/pkgconfig:$(brew --prefix openssl@3)/lib/pkgconfig:$(brew --prefix curl)/lib/pkgconfig:$(brew --prefix zlib)/lib/pkgconfig"
```

Windows: use the MSYS2 UCRT64 shell and install the MinGW toolchain, Qt 6,
ngspice, pkgconf, and the libraries listed above. The commands are listed in
[`docs/developer/building.md`](docs/developer/building.md).

## Build

```sh
xmake build doctest openvlsi-cli openvlsi-pkg openvlsi-mcp
```

Build the desktop app (requires Qt 6):

```sh
xmake build openvlsi
```

On Linux, the C++ OpenROAD helper (`openvlsi-openroad`) is built automatically for
the physical design flow; the pinned OpenROAD/OpenSTA source pair is fetched
into `$HOME/.cache/openvlsi/openroad-sdk` on the first build. On Windows and macOS,
disable it with `OPENVLSI_ENABLE_OPENROAD=0`:

```sh
OPENVLSI_ENABLE_OPENROAD=0 xmake build openvlsi openvlsi-cli openvlsi-pkg openvlsi-mcp
```

Build or rebuild the helper on Linux with:

```sh
xmake build openvlsi-openroad
```

This requires `bazelisk` or `bazel` on `PATH`. Set
`OPENVLSI_OPENROAD_SRC=/path/to/OpenROAD` to build against an existing source
checkout instead.

## Package

```sh
xmake pack -f targz,zip   # Linux
xmake pack -f zip         # macOS
xmake pack -f nsis,zip    # Windows (MSYS2 UCRT64 + NSIS)
```

The Windows installer is generated with XPack's NSIS backend and bundles the
Qt/MinGW runtime. macOS packages include an `OpenVLSI.app` bundle. See
`.github/workflows/release.yml` for the three-platform release pipeline.

## Test

```sh
xmake run doctest
```

or:

```sh
xmake test
```

Run the pure-Python simulation metrics tests:

```sh
PYTHONPATH=python python3 tests/openvlsipy_sim_tests.py
python3 tests/openvlsipy_report_runner_tests.py
```

Format first-party C++ sources:

```sh
xmake format
```

## Documentation

Full documentation is in [`docs/`](docs/) (Sphinx, Furo theme with an
xmake-docs-inspired palette). It is split into a user guide, a developer guide,
and project direction pages; start at [`docs/index.md`](docs/index.md). Build it
with:

```sh
pip install -r docs/requirements.txt
sphinx-build -b html docs docs/_build
```

or:

```sh
xmake docs
```

| Section | Contents |
| --- | --- |
| User Guide | Getting started, GUI, CLI, simulation, symbols, packages, Python API, procedural layouts, MCP, and agent recipes. |
| Developer Guide | Build/test workflow, architecture, capability parity, package/PDK authoring, plugins, and the documentation guide. |
| Project | Product vision and implementation roadmap. |

## Project status

Early development. File formats, plugin APIs, and package schemas may break
between versions without migration paths; version stamps are recorded so
breakage is detectable. See the roadmap in `docs/` for the current scope.

## License

MIT. See [LICENSE](LICENSE).
