
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
| `icepy` | Optional Python bindings (`nanobind`) for project automation, layout building, and PDK PCell generation. |

Key integrations (behind adapter boundaries): Qt 6, Clipper2 geometry,
ngspice (`libngspice`), ALIGN, GDSII/OASIS interchange, TOML packages,
Python scientific stack.

## Principles

- Core-first: the GUI is a client of `icelib`, not the primary abstraction.
- Manual, scripted, and AI-assisted edits operate on the same layout database.
- External tools are accessed through versioned adapters; their types do not leak into the core model.
- Results are deterministic and reproducible from declared inputs and recorded versions.

## Project layout

An ICE project is a directory:

```text
project/
├── project.icepro       # project metadata
├── sch/                 # *.icesch schematic cells
├── lay/                 # *.icelay layout cells and procedural layouts
├── sim/                 # *.icesim simulations
├── scripts/             # project-level Python simulation scripts
├── build/               # generated simulation runs and build output, ignored by Git
├── .gitignore
└── AGENTS.md
```

All project files are JSON except simulation scripts. `project.icepro` contains
only project-level metadata, such as identity, name, units, grid, package
dependencies, layers, and simulation script associations. Schematics, layouts, and
simulations are stored as individual files.

## Prerequisites

ICE builds on Linux, macOS, and Windows. Dependencies are resolved through
`pkg-config`.

- C++20 compiler
- [xmake](https://xmake.io/) 3.1+ (build system, see `xmake.lua`)
- Qt 6 (Widgets, Gui, Core, Svg, SvgWidgets) for the `ice` target
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
brew install qt libngspice tomlplusplus spdlog nlohmann-json openssl@3 \
    curl zlib simdjson pkgconf
export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig:$(brew --prefix openssl@3)/lib/pkgconfig:$(brew --prefix curl)/lib/pkgconfig:$(brew --prefix zlib)/lib/pkgconfig"
```

Windows: use the MSYS2 UCRT64 shell and install the MinGW toolchain, Qt 6,
ngspice, pkgconf, and the libraries listed above. The commands are listed in
[`docs/developer/building.md`](docs/developer/building.md).

## Build

```sh
xmake build doctest ice-cli ice-pkg ice-mcp
```

Build the desktop app (requires Qt 6):

```sh
xmake build ice
```

On Linux, the C++ OpenROAD helper (`ice-openroad`) is built automatically for
the physical design flow; the pinned OpenROAD/OpenSTA source pair is fetched
into `$HOME/.cache/ice/openroad-sdk` on the first build. On Windows and macOS,
disable it with `ICE_ENABLE_OPENROAD=0`:

```sh
ICE_ENABLE_OPENROAD=0 xmake build ice ice-cli ice-pkg ice-mcp
```

Build or rebuild the helper on Linux with:

```sh
xmake build ice-openroad
```

This requires `bazelisk` or `bazel` on `PATH`. Set
`ICE_OPENROAD_SRC=/path/to/OpenROAD` to build against an existing source
checkout instead.

## Package

```sh
xmake pack -f targz,zip   # Linux
xmake pack -f zip         # macOS
xmake pack -f nsis,zip    # Windows (MSYS2 UCRT64 + NSIS)
```

The Windows installer is generated with XPack's NSIS backend and bundles the
Qt/MinGW runtime. macOS packages include an `ICE.app` bundle. See
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
PYTHONPATH=python python3 tests/icepy_sim_tests.py
python3 tests/icepy_report_runner_tests.py
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
