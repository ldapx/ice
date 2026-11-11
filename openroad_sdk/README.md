# ice_openroad C++ backend

This directory contains the C++ OpenROAD backend introduced by
`OPENROAD_CPP_LIBRARY_PROPOSAL.md`.

## Components

- `include/ice_openroad/types.hpp` — stage job/result types.
- `include/ice_openroad/engine.hpp` — stage and heat-map engine API.
- `include/ice_openroad/odb_reader.hpp` — read-only ODB summary/preview API.
- `include/ice_openroad/heatmap.hpp` — native OpenROAD heat-map export types.
- `src/engine.cpp` — OpenROAD/OpenDB implementation of the physical stages
  and native heat-map export.
- `src/odb_reader.cpp` — OpenDB-only summary, preview, and embedded density
  maps. It does not initialize OpenROAD or OpenSTA.
- `src/worker_main.cpp` — process-isolated `ice-openroad` harness.
- `BUILD.bazel` — static library, shared library, and harness targets.

The library owns OpenROAD initialization. The harness links the library and
provides hard cancellation and process isolation for stage execution.

## Building

The helper must be built against an OpenROAD source checkout. See
`scripts/build-ice-openroad.sh`.

```sh
ICE_OPENROAD_SRC=/path/to/OpenROAD scripts/build-ice-openroad.sh
```

or:

```sh
ICE_OPENROAD_SRC=/path/to/OpenROAD xmake openroad-sdk
```

The script copies the helper next to `ice-cli` in the ICE build directory.
Override discovery with `ICE_OPENROAD_HELPER=/path/to/ice-openroad`.

### Installable SDK

Use `--sdk-install <prefix>` to install the shared library, public headers,
harness, and a `pkg-config` file:

```sh
ICE_OPENROAD_SRC=/path/to/OpenROAD \
  scripts/build-ice-openroad.sh --sdk-install "$HOME/.local/ice-openroad"
```

```sh
PKG_CONFIG_PATH="$HOME/.local/ice-openroad/lib/pkgconfig" \
  pkg-config --cflags --libs ice_openroad
```

The installed layout is:

```text
<prefix>/bin/ice-openroad
<prefix>/include/ice_openroad/*.hpp
<prefix>/lib/libice_openroad.so
<prefix>/lib/pkgconfig/ice_openroad.pc
```

The shared library is built by the Bazel target
`//ice_openroad:ice_openroad_shared`. It is intended for tool integrations
built with the same OpenROAD/Bazel toolchain. ICE itself continues to use the
process-isolated harness by default so a crash or OOM in OpenROAD cannot take
down the GUI and so the GUI does not acquire a hard runtime ABI dependency.

## Harness commands

Physical stages:

```text
ice-openroad floorplan [...]
ice-openroad placement [...]
ice-openroad cts [...]
ice-openroad routing [...]
ice-openroad final [...]
```

Read-only ODB access:

```text
ice-openroad odb-info db/3_place.odb
ice-openroad odb-preview db/5_route.odb --heatmap-bins 32
```

Both write JSON to stdout. `odb-preview` contains the die bounds, routed and
special-net geometry, instance rectangles, block pins, and embedded placement
density and routing-density grids. ICE uses this command for read-only stage
previews and falls back to the legacy DEF import when the helper is missing or
the ODB cannot be read.

Native OpenROAD heat maps:

```text
ice-openroad heatmap --input-db db/5_route.odb \
  --liberty stdcell.lib.gz --sdc final/5_route.sdc \
  --kind routing --output reports/routing-heatmap.csv
```

Supported kinds are `placement`, `routing`, `power`, `pin`, `rudy`, and
`irdrop`. ICE exposes overlay toggles for the embedded placement/routing maps
and an **Export CSV** action for the native maps.

## Backend selection

`icelib` uses the C++ backend automatically when `ice-openroad` is available.
The legacy Tcl backend can be forced for debugging or during migration:

```sh
ICE_OPENROAD_BACKEND=tcl xmake run ice
ICE_OPENROAD_BACKEND=tcl ice-cli implementation-run ...
```

Force the C++ backend with:

```sh
ICE_OPENROAD_BACKEND=cpp ice-cli implementation-run ...
```

If the helper is missing, ICE falls back to the Tcl backend in `auto` mode.

## Current status

Implemented C++ stages and parity steps:

- floorplan;
- placement, including `repair_design` and placement parasitics;
- CTS, including propagated clocks, `repair_clock_nets`, and detailed
  placement;
- routing, including guide output, antenna checking, detailed route, filler
  placement, placement checking, and global connect;
- final, including OpenRCX extraction, SPEF write/read-back, and timing/power
  metrics.

Still using Tcl inside the library for:

- SDC reading and writing;
- OpenROAD initialization;
- platform tapcell/PDN hooks.

Remaining work:

- richer metrics/report exports;
- in-process execution for short stages;
- packaging the shared SDK alongside the OpenROAD AUR split package.
