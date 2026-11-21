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

ICE builds on Linux, macOS, and Windows. All C/C++ dependencies are resolved
through `pkg-config`, so the same `xmake.lua` works on every platform. Linux
and macOS use their native package managers; Windows uses the MSYS2 UCRT64
environment.

Common prerequisites:

- C++20 compiler
- [xmake](https://xmake.io/) 3.1+ (build system, see `xmake.lua`)
- `pkg-config` (or `pkgconf`)
- Qt 6 (Widgets, Gui, Core, Svg, SvgWidgets) for the `ice` target
- `libngspice`, `toml++`, `spdlog`, `nlohmann_json`, OpenSSL, zlib
- `clang-format` for `xmake format`
- The bundled `agentsdk-cpp` submodule. Clone with `--recursive`, or run
  `git submodule update --init --recursive` after cloning. The SDK is linked
  statically into the ICE executables.

`clipper2` is installed by xmake from source, so a broken distribution-provided
Clipper2 archive cannot break the link.

### Linux

```sh
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
    qt6-base-dev qt6-svg-dev libngspice0-dev libtomlplusplus-dev \
    libssl-dev zlib1g-dev libcurl4-openssl-dev
```

### macOS

```sh
brew install qtbase qtsvg libngspice tomlplusplus spdlog nlohmann-json \
    openssl@3 curl zlib simdjson pkgconf
export PKG_CONFIG_PATH="$(brew --prefix qtbase)/lib/pkgconfig:$(brew --prefix qtsvg)/lib/pkgconfig:$(brew --prefix)/lib/pkgconfig:$(brew --prefix openssl@3)/lib/pkgconfig:$(brew --prefix curl)/lib/pkgconfig:$(brew --prefix zlib)/lib/pkgconfig"
```

The official Qt macOS SDK is a framework build without `.pc` files. Set
`ICE_QT_PREFIX` to that SDK (for example
`$HOME/Qt/6.8.3/macos` from `aqtinstall`) and ICE will link the Qt frameworks
directly.

### Windows (MSYS2 UCRT64)

Install the UCRT64 toolchain and dependencies from an MSYS2 UCRT64 shell:

```sh
pacman -S --needed git pkgconf \
    mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-xmake \
    mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-svg \
    mingw-w64-ucrt-x86_64-ngspice mingw-w64-ucrt-x86_64-tomlplusplus \
    mingw-w64-ucrt-x86_64-spdlog mingw-w64-ucrt-x86_64-nlohmann-json \
    mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-zlib \
    mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-simdjson \
    mingw-w64-ucrt-x86_64-nsis mingw-w64-ucrt-x86_64-ntldd
```

Build from the UCRT64 shell with the MinGW target. MSYS targets are treated
as ELF hosts by xmake's packaging code, so configure explicitly:

```sh
xmake f -p mingw -a x86_64 --mingw="$(cygpath -w "$MINGW_PREFIX")" -m release
```

### OpenROAD

OpenROAD's C++ helper is built with Bazel and Bash, so `ice-openroad` is
Linux-only by default. Disable it on other platforms (the rest of ICE builds
normally):

```sh
ICE_ENABLE_OPENROAD=0 xmake build ice ice-cli ice-pkg ice-mcp
```

Force it on an unsupported host with `ICE_ENABLE_OPENROAD=1` only when you
have provided a compatible `ice-openroad` helper yourself.

Optional targets can be disabled with `ICE_BUILD_GUI=0` and
`ICE_BUILD_PYTHON=0`. Python bindings additionally accept
`ICE_PYTHON_VERSION`, `ICE_PYTHON_INCLUDE`, `ICE_PYTHON_LIBDIR`,
`ICE_PYTHON_LIB`, and `NANOBIND_ROOT`.

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

On Linux, building `ice`, `ice-cli`, or `ice-mcp` also builds the required
`ice-openroad` helper when it is missing. To build it on its own:

```sh
xmake build ice-openroad
```

Force a helper rebuild with `xmake build -r ice-openroad`.

## Package

XPack is included in `xmake.lua`. The generated package contains the GUI, CLI,
package manager, MCP server, symbols, and runtime assets:

```sh
# Linux
xmake pack -f targz,zip

# macOS
xmake pack -f zip

# Windows (from MSYS2 UCRT64; NSIS must be installed)
xmake pack -f nsis,zip
```

The Windows packaging step runs `windeployqt`, copies the MinGW/MSYS2 runtime
DLLs and ngspice code models, and writes an NSIS installer. The macOS step
creates `ICE.app` and deploys Qt frameworks; install `dylibbundler` to bundle
non-Qt libraries as well.

The `.github/workflows/release.yml` workflow builds Linux x86_64, macOS
(arm64 and x86_64), and Windows x86_64 packages. Pushing a `v*` tag publishes the resulting archives
and Windows installer to the GitHub release.

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
