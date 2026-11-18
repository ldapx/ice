"""Built-in procedural-layout cell generators.

The procedural runner loads this module lazily. Every generator receives the
group context ``ctx`` (a :class:`procedural_runner.Group`) and returns a handle
that can be added to a row, column, grid, or layout. Generators deliberately
use only the public builder API, so package generators can follow the same
contract.
"""

from __future__ import annotations


def _number(value, default=0):
    if value is None:
        return default
    return int(round(float(value)))


def _nets(nets):
    return dict(nets or {})


def rail(
    ctx,
    layer="Metal1",
    x=0,
    y=0,
    width=1000,
    height=200,
    net=None,
    name="RAIL",
):
    """Draw a single power/ground rail with a stable name and net."""
    return ctx.rectangle(layer, x, y, width, height, name=name, net=net)


def via_stack(
    ctx,
    layers=None,
    x=0,
    y=0,
    size=200,
    width=None,
    height=None,
    net=None,
    name="VIA",
):
    """Draw one rectangle on each layer to form a via stack."""
    layers = list(layers or ["Metal1", "Via1", "Metal2"])
    group = ctx.group(name)
    group.via(
        layers,
        x,
        y,
        size=size,
        width=width,
        height=height,
        name=name,
        net=net,
    )
    return group


def guard_ring(
    ctx,
    layer="Metal1",
    around=None,
    x=0,
    y=0,
    width=None,
    height=None,
    ring=200,
    net=None,
    name="RING",
):
    """Draw a rectangular ring around an item or an explicit box."""
    if around is not None:
        bounds = around.bounds if hasattr(around, "bounds") else None
        if callable(bounds):
            bounds = bounds()
        if isinstance(bounds, dict):
            bounds = (
                bounds.get("x", 0),
                bounds.get("y", 0),
                bounds.get("x", 0) + bounds.get("width", 0),
                bounds.get("y", 0) + bounds.get("height", 0),
            )
        if not bounds:
            raise ValueError("guard_ring around= item has no bounds")
        x0, y0, x1, y1 = bounds
        width = x1 - x0
        height = y1 - y0
        x = x0
        y = y0
    else:
        x0, y0 = _number(x), _number(y)
        x1 = x0 + _number(width)
        y1 = y0 + _number(height)
        width = x1 - x0
        height = y1 - y0
    ring = max(1, _number(ring, 200))
    x = _number(x)
    y = _number(y)
    width = max(1, _number(width))
    height = max(1, _number(height))
    group = ctx.group(name)
    group.rectangle(
        layer,
        x - ring,
        y - ring,
        width + 2 * ring,
        ring,
        name=f"{name}_bottom",
        net=net,
    )
    group.rectangle(
        layer, x - ring, y + height, width + 2 * ring, ring, name=f"{name}_top", net=net
    )
    group.rectangle(layer, x - ring, y, ring, height, name=f"{name}_left", net=net)
    group.rectangle(layer, x + width, y, ring, height, name=f"{name}_right", net=net)
    return group


def resistor(
    ctx,
    device="resistor",
    x=0,
    y=0,
    orientation="r0",
    name="R1",
    nets=None,
    **parameters,
):
    """Place one package resistor PCell."""
    return ctx.device(
        device,
        x=x,
        y=y,
        orientation=orientation,
        name=name,
        nets=_nets(nets),
        **parameters,
    )


def resistor_bank(
    ctx,
    device="resistor",
    count=2,
    spacing=0,
    orientation="r0",
    prefix="R",
    nets=None,
    **parameters,
):
    """Place a horizontal bank of resistors, abutting by default."""
    row = ctx.row(gap=spacing, align="center")
    for index in range(max(1, int(count))):
        row.add(
            ctx.device(
                device,
                x=0,
                y=0,
                orientation=orientation,
                name=f"{prefix}{index + 1}",
                nets=_nets(nets),
                **parameters,
            )
        )
    return row


def cap(
    ctx,
    device="capacitor",
    x=0,
    y=0,
    orientation="r0",
    name="C1",
    nets=None,
    **parameters,
):
    """Place one package capacitor PCell."""
    return ctx.device(
        device,
        x=x,
        y=y,
        orientation=orientation,
        name=name,
        nets=_nets(nets),
        **parameters,
    )


def cap_array(
    ctx,
    device="capacitor",
    count=2,
    columns=1,
    x_gap=0,
    y_gap=0,
    prefix="C",
    nets=None,
    **parameters,
):
    """Place a common-centroid-friendly grid of unit capacitors."""
    grid = ctx.grid(
        columns=max(1, int(columns)), x_gap=x_gap, y_gap=y_gap, align="center"
    )
    for index in range(max(1, int(count))):
        grid.add(
            ctx.device(
                device,
                x=0,
                y=0,
                name=f"{prefix}{index + 1}",
                nets=_nets(nets),
                **parameters,
            )
        )
    return grid


def mos(
    ctx,
    device="nmos",
    x=0,
    y=0,
    orientation="r0",
    name="M1",
    nets=None,
    **parameters,
):
    """Place one package MOS PCell or finger bank."""
    return ctx.device(
        device,
        x=x,
        y=y,
        orientation=orientation,
        name=name,
        nets=_nets(nets),
        **parameters,
    )


def mos_pair(
    ctx,
    device="nmos",
    pattern="cc",
    shared_source=True,
    guard_ring_layer=None,
    guard_ring_width=200,
    left_name="M1",
    right_name="M2",
    nets=None,
    **parameters,
):
    """Place a matched pair with optional mirroring and a guard ring.

    ``pattern`` accepts ``single``, ``id`` (interdigitated orientation), and
    ``cc`` (common-centroid mirroring). The two devices abut by default so the
    common source/drain region can be shared by the router.
    """
    row = ctx.row(gap=0, align="center")
    left = ctx.device(device, name=left_name, nets=_nets(nets), **parameters)
    right = ctx.device(device, name=right_name, nets=_nets(nets), **parameters)
    if str(pattern).lower() in {"cc", "id", "common_centroid", "interdigitated"}:
        right.mirror("y")
    row.add(left)
    row.add(right)
    if guard_ring_layer:
        result = ctx.group(f"{left_name}_{right_name}")
        result.add(row)
        result.add(
            guard_ring(
                ctx,
                layer=guard_ring_layer,
                around=row,
                ring=guard_ring_width,
                name=f"{left_name}_{right_name}_RING",
            )
        )
        return result
    return row


def route_bundle(
    ctx,
    layer="Metal1",
    count=2,
    x=0,
    y=0,
    pitch=400,
    length=2000,
    width=100,
    axis="x",
    net=None,
    name="BUNDLE",
):
    """Draw a set of parallel route corridors."""
    group = ctx.group(name)
    for index in range(max(1, int(count))):
        if str(axis).lower() in {"x", "horizontal", "h"}:
            points = [
                (x, y + index * _number(pitch)),
                (x + _number(length), y + index * _number(pitch)),
            ]
        else:
            points = [
                (x + index * _number(pitch), y),
                (x + index * _number(pitch), y + _number(length)),
            ]
        group.path(layer, points, width=width, name=f"{name}_{index + 1}", net=net)
    return group


def _entry(function, **metadata):
    metadata.setdefault("name", function.__name__)
    metadata.setdefault("description", (function.__doc__ or "").strip())
    metadata.setdefault("parameters", {})
    metadata.setdefault("ports", [])
    metadata["source"] = "builtin"
    metadata["version"] = 1
    return function, metadata


def generators():
    """Return the built-in generator registry as ``name -> (callable, meta)``."""
    entries = [
        _entry(
            rail,
            parameters={
                "layer": "Metal1",
                "x": 0,
                "y": 0,
                "width": 1000,
                "height": 200,
                "net": None,
                "name": "RAIL",
            },
            category="Power",
        ),
        _entry(
            via_stack,
            parameters={
                "layers": ["Metal1", "Via1", "Metal2"],
                "x": 0,
                "y": 0,
                "size": 200,
                "net": None,
                "name": "VIA",
            },
            category="Routing",
        ),
        _entry(
            guard_ring,
            parameters={"layer": "Metal1", "ring": 200, "net": None, "name": "RING"},
            category="Matching",
        ),
        _entry(
            resistor,
            parameters={
                "device": "resistor",
                "x": 0,
                "y": 0,
                "orientation": "r0",
                "name": "R1",
                "nets": {},
            },
            category="Devices",
        ),
        _entry(
            resistor_bank,
            parameters={
                "device": "resistor",
                "count": 2,
                "spacing": 0,
                "orientation": "r0",
                "prefix": "R",
                "nets": {},
            },
            category="Devices",
        ),
        _entry(
            cap,
            parameters={
                "device": "capacitor",
                "x": 0,
                "y": 0,
                "orientation": "r0",
                "name": "C1",
                "nets": {},
            },
            category="Devices",
        ),
        _entry(
            cap_array,
            parameters={
                "device": "capacitor",
                "count": 2,
                "columns": 1,
                "x_gap": 0,
                "y_gap": 0,
                "prefix": "C",
                "nets": {},
            },
            category="Matching",
        ),
        _entry(
            mos,
            parameters={
                "device": "nmos",
                "x": 0,
                "y": 0,
                "orientation": "r0",
                "name": "M1",
                "nets": {},
            },
            category="Devices",
        ),
        _entry(
            mos_pair,
            parameters={
                "device": "nmos",
                "pattern": "cc",
                "shared_source": True,
                "guard_ring_layer": None,
                "guard_ring_width": 200,
                "left_name": "M1",
                "right_name": "M2",
                "nets": {},
            },
            category="Matching",
        ),
        _entry(
            route_bundle,
            parameters={
                "layer": "Metal1",
                "count": 2,
                "x": 0,
                "y": 0,
                "pitch": 400,
                "length": 2000,
                "width": 100,
                "axis": "x",
                "net": None,
                "name": "BUNDLE",
            },
            category="Routing",
        ),
    ]
    return {metadata["name"]: (function, metadata) for function, metadata in entries}
