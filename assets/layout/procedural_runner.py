#!/usr/bin/env python3
"""Run an ICE procedural layout script and emit generated geometry as JSON.

The user script is executed with a single injected object named ``layout``.
It builds an in-memory description of the generated static layout. ICE parses
the resulting JSON, validates it, and applies it to the project's layout
database.

Coordinates are integer database units (one nanometer by default). Use
``layout.nm(...)``, ``layout.um(...)``, ``layout.db(...)``, and
``layout.snap(...)`` to work in physical units.

The public script-facing API is documented on the
:doc:`procedural layout API </user/procedural-layout>` page. Scripts should normally
use :class:`Layout`, :class:`Layer`, :class:`Shape`,
:class:`DevicePlacement`, and :class:`Route`; composition helpers return
:class:`Group`, :class:`Row`, :class:`Column`, :class:`Grid`, :class:`Stack`,
and :class:`Repeat` handles. The remaining names in this module are runner
internals.
"""

from __future__ import annotations

import argparse
import copy
import heapq
import importlib.util
import json
import math
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import traceback


SCHEMA_VERSION = 2
BUILDER_VERSION = 4


class LayoutQualityError(RuntimeError):
    """Raised when the placement compaction gate or review does not pass."""


def _coordinate(value, label):
    try:
        number = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{label} must be a number") from error
    if not math.isfinite(number):
        raise ValueError(f"{label} must be finite")
    return int(round(number))


def _positive(value, label):
    number = _coordinate(value, label)
    if number <= 0:
        raise ValueError(f"{label} must be greater than zero")
    return number


def _point(value, label):
    if isinstance(value, dict):
        x = value.get("x")
        y = value.get("y")
    else:
        try:
            x, y = value
        except (TypeError, ValueError) as error:
            raise ValueError(f"{label} must be (x, y)") from error
    return _coordinate(x, f"{label} x"), _coordinate(y, f"{label} y")


def _shape_points(shape):
    if "rectangle" in shape:
        rectangle = shape["rectangle"]
        x = rectangle["x"]
        y = rectangle["y"]
        width = rectangle["width"]
        height = rectangle["height"]
        return [(x, y), (x + width, y), (x + width, y + height), (x, y + height)]
    return [(point["x"], point["y"]) for point in shape.get("polygon", [])]


def _shape_record_bounds(record):
    """Return ``(x, y, width, height)`` for one shape record."""
    if not isinstance(record, dict):
        return None
    if "rectangle" in record:
        rectangle = record["rectangle"]
        return (
            int(rectangle.get("x", 0)),
            int(rectangle.get("y", 0)),
            int(rectangle.get("width", 0)),
            int(rectangle.get("height", 0)),
        )
    if "polygon" in record and record["polygon"]:
        points = record["polygon"]
        xs = [int(point.get("x", 0)) for point in points]
        ys = [int(point.get("y", 0)) for point in points]
        return (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))
    if "centerline" in record and record["centerline"]:
        width = int(record.get("width", 0))
        half = (width + 1) // 2
        points = record["centerline"]
        xs = [int(point.get("x", 0)) for point in points]
        ys = [int(point.get("y", 0)) for point in points]
        return (
            min(xs) - half,
            min(ys) - half,
            max(xs) - min(xs) + 2 * half,
            max(ys) - min(ys) + 2 * half,
        )
    if "boolean" in record:
        boxes = [
            _shape_record_bounds(operand)
            for operand in record.get("boolean", {}).get("operands", [])
        ]
        boxes = [box for box in boxes if box is not None]
        if boxes:
            x0 = min(box[0] for box in boxes)
            y0 = min(box[1] for box in boxes)
            x1 = max(box[0] + box[2] for box in boxes)
            y1 = max(box[1] + box[3] for box in boxes)
            return (x0, y0, x1 - x0, y1 - y0)
    return None


def _translate_record(record, dx, dy):
    """Translate one shape record in place."""
    if not isinstance(record, dict):
        return
    if "rectangle" in record:
        record["rectangle"]["x"] += dx
        record["rectangle"]["y"] += dy
    for point in record.get("polygon", []):
        point["x"] += dx
        point["y"] += dy
    for hole in record.get("holes", []):
        for point in hole:
            point["x"] += dx
            point["y"] += dy
    for point in record.get("centerline", []):
        point["x"] += dx
        point["y"] += dy
    if "boolean" in record:
        for operand in record["boolean"].get("operands", []):
            _translate_record(operand, dx, dy)


def _mirror_record(record, flip_x, flip_y, low_x, high_x, low_y, high_y):
    """Mirror one shape record in place within the given bounds."""
    if not isinstance(record, dict):
        return
    if "rectangle" in record:
        rectangle = record["rectangle"]
        if flip_x:
            rectangle["x"] = low_x + high_x - rectangle["x"] - rectangle["width"]
        if flip_y:
            rectangle["y"] = low_y + high_y - rectangle["y"] - rectangle["height"]
    for point in record.get("polygon", []):
        if flip_x:
            point["x"] = low_x + high_x - point["x"]
        if flip_y:
            point["y"] = low_y + high_y - point["y"]
    for hole in record.get("holes", []):
        for point in hole:
            if flip_x:
                point["x"] = low_x + high_x - point["x"]
            if flip_y:
                point["y"] = low_y + high_y - point["y"]
    for point in record.get("centerline", []):
        if flip_x:
            point["x"] = low_x + high_x - point["x"]
        if flip_y:
            point["y"] = low_y + high_y - point["y"]
    if "boolean" in record:
        for operand in record["boolean"].get("operands", []):
            _mirror_record(operand, flip_x, flip_y, low_x, high_x, low_y, high_y)


def _mirror_side(side, flip_x, flip_y):
    if flip_x and side == "left":
        return "right"
    if flip_x and side == "right":
        return "left"
    if flip_y and side == "top":
        return "bottom"
    if flip_y and side == "bottom":
        return "top"
    return side


def _mirror_orientation(orientation, flip_x, flip_y):
    # flip_x negates x (mirror about the Y axis); flip_y negates y (mirror
    # about the X axis). Compose that reflection with the existing placement.
    if flip_x and not flip_y:
        return {
            "r0": "my",
            "my": "r0",
            "r90": "m45",
            "m45": "r90",
            "r180": "mx",
            "mx": "r180",
            "r270": "m135",
            "m135": "r270",
        }.get(orientation, orientation)
    if flip_y and not flip_x:
        return {
            "r0": "mx",
            "mx": "r0",
            "r90": "m135",
            "m135": "r90",
            "r180": "my",
            "my": "r180",
            "r270": "m45",
            "m45": "r270",
        }.get(orientation, orientation)
    if flip_x and flip_y:
        return {
            "r0": "r180",
            "r180": "r0",
            "r90": "r270",
            "r270": "r90",
            "mx": "my",
            "my": "mx",
            "m45": "m135",
            "m135": "m45",
        }.get(orientation, orientation)
    return orientation


def _orientation_point(orientation, x, y):
    """Apply a placement orientation about the origin."""
    orientation = str(orientation or "r0")
    if orientation == "r0":
        return x, y
    if orientation == "r90":
        return -y, x
    if orientation == "r180":
        return -x, -y
    if orientation == "r270":
        return y, -x
    if orientation == "mx":
        return x, -y
    if orientation == "my":
        return -x, y
    if orientation == "m45":
        return y, x
    if orientation == "m135":
        return -y, -x
    raise ValueError(f"Unsupported orientation: {orientation}")


def _orientation_side(side, orientation):
    """Return the side after applying a placement orientation."""
    side = str(side or "left").lower()
    if orientation in {"r0", ""}:
        return side
    if orientation in {"r90", "m45"}:
        return {
            "left": "bottom",
            "right": "top",
            "top": "left",
            "bottom": "right",
        }.get(side, side)
    if orientation == "r180":
        return {
            "left": "right",
            "right": "left",
            "top": "bottom",
            "bottom": "top",
        }.get(side, side)
    if orientation == "r270":
        return {
            "left": "top",
            "right": "bottom",
            "top": "right",
            "bottom": "left",
        }.get(side, side)
    if orientation == "mx":
        return {"top": "bottom", "bottom": "top"}.get(side, side)
    if orientation == "my":
        return {"left": "right", "right": "left"}.get(side, side)
    if orientation == "m135":
        return {
            "left": "top",
            "right": "bottom",
            "top": "left",
            "bottom": "right",
        }.get(side, side)
    return side


def _instance_local_bounds(record):
    """Return the referenced cell's bounds relative to its origin."""
    bounds = record.get("cell_bounds")
    if isinstance(bounds, dict):
        return (
            int(bounds.get("x", 0)),
            int(bounds.get("y", 0)),
            int(bounds.get("width", 0)),
            int(bounds.get("height", 0)),
        )
    return (0, 0, 1, 1)


def _instance_world_bounds(record):
    """Return absolute ``(x0, y0, x1, y1)`` bounds for an instance record."""
    x = int(record.get("x", 0))
    y = int(record.get("y", 0))
    bx, by, width, height = _instance_local_bounds(record)
    points = (
        (bx, by),
        (bx + width, by),
        (bx + width, by + height),
        (bx, by + height),
    )
    transformed = [
        _orientation_point(record.get("orientation", "r0"), px, py) for px, py in points
    ]
    xs = [point[0] for point in transformed]
    ys = [point[1] for point in transformed]
    return (min(xs) + x, min(ys) + y, max(xs) + x, max(ys) + y)


def _transform_instance_rectangle(record, rectangle):
    """Transform a child port rectangle into the parent coordinate system."""
    rect = _route_rect(rectangle)
    if rect is None:
        return None
    x, y, width, height = rect
    points = ((x, y), (x + width, y), (x + width, y + height), (x, y + height))
    transformed = [
        _orientation_point(record.get("orientation", "r0"), px, py) for px, py in points
    ]
    xs = [point[0] for point in transformed]
    ys = [point[1] for point in transformed]
    offset_x = int(record.get("x", 0))
    offset_y = int(record.get("y", 0))
    min_x = min(xs) + offset_x
    min_y = min(ys) + offset_y
    return (min_x, min_y, max(xs) - min(xs), max(ys) - min(ys))


def _instance_port_info(record, cell_info):
    """Return absolute port metadata for an instance and its cell catalog entry."""
    result = {}
    for port in (cell_info or {}).get("ports", []):
        name = str(port.get("name", ""))
        if not name:
            continue
        rectangle = _transform_instance_rectangle(record, port.get("rectangle"))
        if rectangle is None:
            continue
        result[name] = {
            "rectangle": rectangle,
            "layer": port.get("layer"),
            "direction": port.get("direction", "inout"),
            "side": _orientation_side(
                port.get("side", "left"), record.get("orientation", "r0")
            ),
            "net": port.get("net"),
        }
    return result


def _mirror_ports(ports, flip_x, flip_y, low_x, high_x, low_y, high_y):
    for port in ports:
        rectangle = port.get("rectangle")
        if isinstance(rectangle, dict):
            if flip_x:
                rectangle["x"] = low_x + high_x - rectangle["x"] - rectangle["width"]
            if flip_y:
                rectangle["y"] = low_y + high_y - rectangle["y"] - rectangle["height"]
        elif isinstance(rectangle, (list, tuple)) and len(rectangle) == 4:
            x, y, width, height = rectangle
            if flip_x:
                x = low_x + high_x - x - width
            if flip_y:
                y = low_y + high_y - y - height
            port["rectangle"] = (x, y, width, height)
        if "side" in port:
            port["side"] = _mirror_side(port["side"], flip_x, flip_y)


def _bounds_of_records(shapes, ports=None):
    min_x = math.inf
    min_y = math.inf
    max_x = -math.inf
    max_y = -math.inf
    for record in shapes:
        box = _shape_record_bounds(record)
        if box is None:
            continue
        x, y, width, height = box
        min_x = min(min_x, x)
        min_y = min(min_y, y)
        max_x = max(max_x, x + width)
        max_y = max(max_y, y + height)
    for port in ports or []:
        box = _shape_record_bounds({"rectangle": port.get("rectangle", {})})
        if box is None:
            continue
        x, y, width, height = box
        min_x = min(min_x, x)
        min_y = min(min_y, y)
        max_x = max(max_x, x + width)
        max_y = max(max_y, y + height)
    if not math.isfinite(min_x):
        return None
    return (int(min_x), int(min_y), int(max_x), int(max_y))


def _json_safe(value):
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if isinstance(value, dict):
        return {str(key): _json_safe(entry) for key, entry in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(entry) for entry in value]
    return str(value)


def _union_bounds(boxes):
    boxes = [box for box in boxes if box is not None]
    if not boxes:
        return None
    return (
        min(box[0] for box in boxes),
        min(box[1] for box in boxes),
        max(box[2] for box in boxes),
        max(box[3] for box in boxes),
    )


def _path_segment_polygon(start, end, width):
    x0, y0 = start
    x1, y1 = end
    dx = x1 - x0
    dy = y1 - y0
    length = math.hypot(dx, dy)
    if length == 0:
        return []
    nx = -dy * width / (2.0 * length)
    ny = dx * width / (2.0 * length)
    return [
        (int(round(x0 + nx)), int(round(y0 + ny))),
        (int(round(x1 + nx)), int(round(y1 + ny))),
        (int(round(x1 - nx)), int(round(y1 - ny))),
        (int(round(x0 - nx)), int(round(y0 - ny))),
    ]


class Shape:
    """Handle for a shape added through the layout builder.

    Shapes are returned by :meth:`Layout.rectangle`, :meth:`Layout.polygon`,
    :meth:`Layout.path`, :meth:`Layout.text`, and the boolean helpers. A shape
    is already part of the generated layout when it is returned; the handle
    exposes its editable ``name`` and ``net`` metadata.

    :ivar name: Optional shape name shown in the layout hierarchy.
    :ivar net: Optional layout net name. Routers treat same-net shapes as
        connectable geometry and other-net shapes as obstacles.
    """

    def __init__(self, layout, record):
        self._layout = layout
        self._record = record

    @property
    def bounds(self):
        """Axis-aligned ``(x0, y0, x1, y1)`` bounds, or ``None`` when empty."""
        box = _shape_record_bounds(self._record)
        if box is None:
            return None
        x, y, width, height = box
        return (x, y, x + width, y + height)

    @property
    def width(self):
        """Shape width in database units."""
        bounds = self.bounds
        return 0 if bounds is None else bounds[2] - bounds[0]

    @property
    def height(self):
        """Shape height in database units."""
        bounds = self.bounds
        return 0 if bounds is None else bounds[3] - bounds[1]

    @property
    def is_empty(self):
        """Whether the shape has no geometry."""
        return self.bounds is None

    @property
    def ports(self):
        """Shapes never expose named ports; provided for handle symmetry."""
        return {}

    def pin(self, name):
        """Shapes have no ports; raises ``KeyError``."""
        raise KeyError(f"shape '{self.name}' has no port '{name}'")

    port = pin

    def anchor(self, name="center"):
        """Return an anchor point on the shape bounds."""
        return _anchor_point(self.bounds, name)

    def move(self, dx, dy):
        """Translate the shape in place and return ``self``."""
        _translate_record(
            self._record, _coordinate(dx, "move dx"), _coordinate(dy, "move dy")
        )
        return self

    def mirror(self, axis="y"):
        """Mirror the shape about its own ``x`` or ``y`` axis center."""
        bounds = self.bounds
        if bounds is None:
            return self
        flip_x, flip_y = _axis_flags(axis)
        _mirror_record(
            self._record,
            flip_x,
            flip_y,
            bounds[0],
            bounds[2],
            bounds[1],
            bounds[3],
        )
        return self

    @property
    def name(self):
        """Shape name, or an empty string when the shape is unnamed."""
        return self._record.get("name", "")

    @name.setter
    def name(self, value):
        if value is None or str(value).strip() == "":
            self._record.pop("name", None)
            return
        self._record["name"] = str(value).strip()

    @property
    def net(self):
        """Layout net name, or an empty string when the shape has no net."""
        return self._record.get("net", "")

    @net.setter
    def net(self, value):
        if value is None or str(value).strip() == "":
            self._record.pop("net", None)
            return
        self._record["net"] = str(value).strip()


class DevicePlacement:
    """Handle returned by :meth:`Layout.device`.

    The device geometry is flattened into the generated layout, but this
    object keeps the absolute port rectangles and the device metadata so a
    script can route to the device and LVS can recognize it.

    Port rectangles are available both as attributes and through
    :attr:`ports`, for example ``m1.G`` and ``m1.ports["G"]``. Each port
    rectangle is a ``(x, y, width, height)`` tuple in database units and can
    be passed directly to :meth:`Layout.route`.

    :ivar name: Instance name used to prefix emitted port and shape names.
    :ivar device: Device-library cell name.
    :ivar model: SPICE model name when the device library declares one.
    :ivar parameters: Resolved device parameter values as strings.
    :ivar shapes: Flattened :class:`Shape` handles emitted for the device.
    :ivar ports: Mapping of port name to absolute ``(x, y, width, height)``.
    :ivar port_info: Per-port metadata with ``rectangle``, ``layer``, ``side``,
        ``direction``, and ``net`` keys.
    :ivar bounds: Absolute device bounding box as ``x``, ``y``, ``width``,
        and ``height`` keys.

    Iterating a placement yields its flattened shapes, ``len(placement)`` is
    the shape count, and indexing with a port name returns that port rectangle.
    """

    def __init__(
        self,
        name,
        device,
        model,
        parameters,
        shapes,
        ports,
        port_info,
        bounds,
        layout=None,
        device_record=None,
        port_records=None,
    ):
        self.name = name
        self.device = device
        self.model = model
        self.parameters = parameters
        self.shapes = shapes
        self.ports = ports
        self.port_info = port_info
        self.bounds = bounds
        self._layout = layout
        self._device_record = device_record
        self._port_records = list(port_records or [])

    @property
    def width(self):
        """Device bounding-box width in database units."""
        return 0 if not self.bounds else int(self.bounds.get("width", 0))

    @property
    def height(self):
        """Device bounding-box height in database units."""
        return 0 if not self.bounds else int(self.bounds.get("height", 0))

    @property
    def is_empty(self):
        """Whether the device has no flattened shapes or ports."""
        return not self.shapes and not self.ports

    def pin(self, name):
        """Return the absolute port rectangle for ``name``."""
        return self.ports[str(name)]

    port = pin

    def anchor(self, name="center"):
        """Return an anchor point on the device bounds."""
        if not self.bounds:
            return None
        rectangle = (
            self.bounds["x"],
            self.bounds["y"],
            self.bounds["width"],
            self.bounds["height"],
        )
        return _anchor_point(
            (
                rectangle[0],
                rectangle[1],
                rectangle[0] + rectangle[2],
                rectangle[1] + rectangle[3],
            ),
            name,
        )

    def move(self, dx, dy):
        """Translate the device geometry, ports, and metadata in place."""
        if self._layout is not None:
            self._layout._layout_state["manual_moves"] += 1
        dx = _coordinate(dx, "move dx")
        dy = _coordinate(dy, "move dy")
        for shape in self.shapes:
            shape.move(dx, dy)
        for pin, rectangle in list(self.ports.items()):
            self.ports[pin] = (
                rectangle[0] + dx,
                rectangle[1] + dy,
                rectangle[2],
                rectangle[3],
            )
        for info in self.port_info.values():
            rectangle = info.get("rectangle")
            if rectangle:
                info["rectangle"] = (
                    rectangle[0] + dx,
                    rectangle[1] + dy,
                    rectangle[2],
                    rectangle[3],
                )
        if self.bounds:
            self.bounds["x"] += dx
            self.bounds["y"] += dy
        for record in self._port_records:
            rectangle = record.get("rectangle")
            if isinstance(rectangle, dict):
                rectangle["x"] += dx
                rectangle["y"] += dy
        self._sync_device_record()
        return self

    def mirror(self, axis="y"):
        """Mirror the device geometry and port metadata in place."""
        bounds = self.bounds
        if not bounds:
            return self
        flip_x, flip_y = _axis_flags(axis)
        low_x = bounds["x"]
        low_y = bounds["y"]
        high_x = low_x + bounds["width"]
        high_y = low_y + bounds["height"]
        for shape in self.shapes:
            _mirror_record(shape._record, flip_x, flip_y, low_x, high_x, low_y, high_y)
        mirrored_ports = {}
        mirrored_info = {}
        for pin, rectangle in self.ports.items():
            x, y, width, height = rectangle
            if flip_x:
                x = low_x + high_x - x - width
            if flip_y:
                y = low_y + high_y - y - height
            mirrored_ports[pin] = (x, y, width, height)
            info = self.port_info.get(pin, {})
            mirrored_info[pin] = info
            info["rectangle"] = mirrored_ports[pin]
            info["side"] = _mirror_side(info.get("side", "left"), flip_x, flip_y)
        self.ports = mirrored_ports
        self.port_info = mirrored_info
        for record in self._port_records:
            x, y, width, height = _route_rect(record.get("rectangle"))
            if flip_x:
                x = low_x + high_x - x - width
            if flip_y:
                y = low_y + high_y - y - height
            record["rectangle"] = {
                "x": x,
                "y": y,
                "width": width,
                "height": height,
            }
            record["side"] = _mirror_side(record.get("side", "left"), flip_x, flip_y)
        self._sync_device_record()
        return self

    def refresh(self):
        """Refresh port/bounds caches after composition moved the records."""
        record = self._device_record
        if not isinstance(record, dict):
            return self
        bounds = record.get("bounds")
        if isinstance(bounds, dict):
            self.bounds = {
                "x": bounds.get("x", 0),
                "y": bounds.get("y", 0),
                "width": bounds.get("width", 0),
                "height": bounds.get("height", 0),
            }
        for port in record.get("ports", []):
            pin = str(port.get("name", ""))
            rectangle = port.get("rectangle")
            if not pin or not isinstance(rectangle, dict):
                continue
            self.ports[pin] = (
                rectangle.get("x", 0),
                rectangle.get("y", 0),
                rectangle.get("width", 0),
                rectangle.get("height", 0),
            )
            info = self.port_info.setdefault(pin, {})
            info["rectangle"] = self.ports[pin]
            info.setdefault("layer", port.get("layer", ""))
            info["side"] = port.get("side", info.get("side", "left"))
            info["direction"] = port.get("direction", info.get("direction", "inout"))
            if "net" in port:
                info["net"] = port.get("net")
        return self

    def _sync_device_record(self):
        record = self._device_record
        if not isinstance(record, dict):
            return
        if self.bounds:
            record["bounds"] = {
                "x": self.bounds["x"],
                "y": self.bounds["y"],
                "width": self.bounds["width"],
                "height": self.bounds["height"],
            }
        for pin_record in record.get("ports", []):
            pin = str(pin_record.get("name", ""))
            if pin in self.ports:
                x, y, width, height = self.ports[pin]
                pin_record["rectangle"] = {
                    "x": x,
                    "y": y,
                    "width": width,
                    "height": height,
                }
                info = self.port_info.get(pin, {})
                if info.get("side"):
                    pin_record["side"] = info["side"]

    def __iter__(self):
        return iter(self.shapes)

    def __len__(self):
        return len(self.shapes)

    def __getitem__(self, key):
        if isinstance(key, str):
            return self.ports[key]
        return self.shapes[key]

    def __getattr__(self, name):
        ports = self.__dict__.get("ports", {})
        if name in ports:
            return ports[name]
        raise AttributeError(name)


class LayoutInstancePlacement:
    """Composable handle returned by :meth:`Layout.instance`.

    The instance is already part of the builder. This handle exposes the
    referenced cell's real bounds and ports, and can be moved, mirrored, added
    to rows/columns/grids, or used as a routing endpoint. Cell bounds and port
    metadata come from the project cell catalog supplied by ICE; when the
    catalog is unavailable, composition falls back to a one-unit bounding box.

    :ivar cell: Referenced layout cell name.
    :ivar name: Instance name in the generated layout.
    :ivar x: Instance origin x in database units.
    :ivar y: Instance origin y in database units.
    :ivar orientation: Placement orientation (``r0``, ``r90``, ``r180``,
        ``r270``, ``mx``, ``my``, ``m45``, or ``m135``).
    :ivar bounds: Absolute bounding box as ``x``, ``y``, ``width``, and
        ``height`` keys.
    :ivar ports: Mapping of port name to absolute
        ``(x, y, width, height)`` rectangles.
    :ivar port_info: Per-port metadata with ``rectangle``, ``layer``, ``side``,
        ``direction``, and ``net`` keys.
    """

    def __init__(self, layout, record, cell_info=None):
        self._layout = layout
        self._record = record
        self._cell_info = cell_info or {}

    @property
    def cell(self):
        return str(self._record.get("cell", ""))

    @property
    def name(self):
        return str(self._record.get("name", ""))

    @name.setter
    def name(self, value):
        self._record["name"] = str(value)

    @property
    def x(self):
        return int(self._record.get("x", 0))

    @x.setter
    def x(self, value):
        self._record["x"] = _coordinate(value, "instance x")

    @property
    def y(self):
        return int(self._record.get("y", 0))

    @y.setter
    def y(self, value):
        self._record["y"] = _coordinate(value, "instance y")

    @property
    def orientation(self):
        return str(self._record.get("orientation", "r0"))

    @orientation.setter
    def orientation(self, value):
        orientations = {"r0", "r90", "r180", "r270", "mx", "my", "m45", "m135"}
        if str(value) not in orientations:
            raise ValueError(f"Unsupported orientation: {value}")
        self._record["orientation"] = str(value)

    @property
    def bounds(self):
        """Absolute instance bounding box.

        When the cell catalog has no geometry for the target, the fallback is
        a one-unit square at the instance origin.
        """
        box = _instance_world_bounds(self._record)
        return {
            "x": box[0],
            "y": box[1],
            "width": box[2] - box[0],
            "height": box[3] - box[1],
        }

    @property
    def width(self):
        return int(self.bounds.get("width", 0))

    @property
    def height(self):
        return int(self.bounds.get("height", 0))

    @property
    def is_empty(self):
        """Instances are references, so they are never treated as empty."""
        return False

    @property
    def port_info(self):
        return _instance_port_info(self._record, self._cell_info)

    @property
    def ports(self):
        return {name: info["rectangle"] for name, info in self.port_info.items()}

    def pin(self, name):
        """Return the absolute ``(x, y, width, height)`` port rectangle."""
        return self.ports[str(name)]

    port = pin

    def anchor(self, name="center"):
        """Return an anchor point on the instance bounds."""
        bounds = self.bounds
        rectangle = (
            bounds["x"],
            bounds["y"],
            bounds["x"] + bounds["width"],
            bounds["y"] + bounds["height"],
        )
        return _anchor_point(rectangle, name)

    def move(self, dx, dy):
        """Translate the instance origin in place."""
        if self._layout is not None:
            self._layout._layout_state["manual_moves"] += 1
        self.x = self.x + _coordinate(dx, "move dx")
        self.y = self.y + _coordinate(dy, "move dy")
        return self

    def mirror(self, axis="y"):
        """Mirror the instance placement in place."""
        bounds = self.bounds
        flip_x, flip_y = _axis_flags(axis)
        low_x = bounds["x"]
        high_x = low_x + bounds["width"]
        low_y = bounds["y"]
        high_y = low_y + bounds["height"]
        if flip_x:
            self.x = low_x + high_x - self.x
        if flip_y:
            self.y = low_y + high_y - self.y
        self._record["orientation"] = _mirror_orientation(
            self.orientation, flip_x, flip_y
        )
        return self

    def refresh(self):
        """Compatibility hook; the handle always reads the live record."""
        return self

    def __getitem__(self, key):
        return self.port(key)

    def __getattr__(self, name):
        info = self.__dict__.get("_cell_info") or {}
        for port in info.get("ports", []):
            if str(port.get("name", "")) == name:
                return self.port(name)
        raise AttributeError(name)


class Route:
    """Handle returned by :meth:`Layout.route` and :meth:`Layout.connect`.

    A route is already committed to the generated layout: its metal and via
    shapes are appended to the builder and carry the requested ``net``.
    The handle reports the planned path and geometry so scripts can inspect
    or reuse the result.

    :ivar net: Net name carried by the emitted shapes.
    :ivar layer: Layer of the first route point.
    :ivar width: Route width in database units.
    :ivar points: Ordered ``(x, y)`` route points, including port centers.
    :ivar point_layers: Layer name for each entry in ``points``.
    :ivar segments: Straight segments with ``start``, ``end``, and ``layer``.
    :ivar vias: Via records with ``x``, ``y``, ``lower``, ``upper``,
        ``via_layers``, ``pad_size``, and ``cut_size``.
    :ivar shapes: Emitted :class:`Shape` handles.
    :ivar length: Total Manhattan length in database units.
    :ivar bends: Number of direction changes within a layer.
    :ivar layer_stack: Layer names available to the route.
    :ivar via_count: Number of via transitions.

    Iterating a route yields its emitted shapes and ``len(route)`` is the
    shape count.
    """

    def __init__(
        self,
        net,
        layer,
        width,
        points,
        point_layers,
        segments,
        vias,
        shapes,
        length,
        bends,
        layer_stack,
    ):
        self.net = net
        self.layer = layer
        self.width = width
        self.points = points
        self.point_layers = point_layers
        self.segments = segments
        self.vias = vias
        self.shapes = shapes
        self.length = length
        self.bends = bends
        self.layer_stack = layer_stack
        self.via_count = len(vias)

    def __iter__(self):
        return iter(self.shapes)

    def __len__(self):
        return len(self.shapes)

    @property
    def bounds(self):
        """Axis-aligned ``(x0, y0, x1, y1)`` bounds of the route shapes."""
        boxes = [
            _shape_record_bounds(shape._record)
            for shape in self.shapes
            if isinstance(shape, Shape)
        ]
        boxes = [
            (box[0], box[1], box[0] + box[2], box[1] + box[3])
            for box in boxes
            if box is not None
        ]
        return _union_bounds(boxes)

    @property
    def is_empty(self):
        """Whether the route emitted no shapes."""
        return not self.shapes

    @property
    def ports(self):
        """Routes never expose named ports; provided for handle symmetry."""
        return {}

    def pin(self, name):
        """Routes have no ports; raises ``KeyError``."""
        raise KeyError(f"route '{self.net or ''}' has no port '{name}'")

    port = pin

    def anchor(self, name="center"):
        """Return an anchor point on the route bounds."""
        return _anchor_point(self.bounds, name)

    def move(self, dx, dy):
        """Translate all route shapes and the planned path in place."""
        owner = getattr(self, "_owner", None)
        if owner is not None:
            owner._layout_state["manual_moves"] += 1
        dx = _coordinate(dx, "move dx")
        dy = _coordinate(dy, "move dy")
        for shape in self.shapes:
            shape.move(dx, dy)
        self.points = [(x + dx, y + dy) for x, y in self.points]
        for segment in self.segments:
            segment["start"] = (
                segment["start"][0] + dx,
                segment["start"][1] + dy,
            )
            segment["end"] = (segment["end"][0] + dx, segment["end"][1] + dy)
        for via in self.vias:
            via["x"] += dx
            via["y"] += dy
        self._owner = getattr(self, "_owner", None)
        return self

    def mirror(self, axis="y"):
        """Mirror all route shapes and planned coordinates in place."""
        bounds = self.bounds
        if bounds is None:
            return self
        flip_x, flip_y = _axis_flags(axis)
        low_x, low_y, high_x, high_y = bounds
        for shape in self.shapes:
            _mirror_record(shape._record, flip_x, flip_y, low_x, high_x, low_y, high_y)
        _mirror_route_metadata(self, flip_x, flip_y, low_x, high_x, low_y, high_y)
        return self


class _GridValue(int):
    """Integer project grid that is also callable as ``layout.grid(...)``."""

    def __new__(cls, value, owner):
        instance = int.__new__(cls, max(1, int(value)))
        instance._owner = owner
        return instance

    def __call__(self, *args, **kwargs):
        return self._owner._grid(*args, **kwargs)


def _axis_flags(axis):
    if axis is True:
        return True, False
    text = str(axis or "y").strip().lower()
    if text in {"x", "horizontal", "h"}:
        return False, True
    if text in {"y", "vertical", "v"}:
        return True, False
    raise ValueError("mirror axis must be 'x' or 'y'")


def _anchor_point(bounds, name="center"):
    """Resolve an anchor name against ``(x0, y0, x1, y1)`` bounds."""
    if bounds is None:
        return None
    x0, y0, x1, y1 = bounds
    center_x = (x0 + x1) // 2
    center_y = (y0 + y1) // 2
    name = str(name or "center").strip().lower()
    points = {
        "c": (center_x, center_y),
        "center": (center_x, center_y),
        "n": (center_x, y1),
        "north": (center_x, y1),
        "s": (center_x, y0),
        "south": (center_x, y0),
        "e": (x1, center_y),
        "east": (x1, center_y),
        "w": (x0, center_y),
        "west": (x0, center_y),
        "nw": (x0, y1),
        "ne": (x1, y1),
        "sw": (x0, y0),
        "se": (x1, y0),
        "left": (x0, center_y),
        "right": (x1, center_y),
        "top": (center_x, y1),
        "bottom": (center_x, y0),
    }
    if name not in points:
        raise ValueError(
            f"unknown anchor '{name}'; use c, n, s, e, w, nw, ne, sw, or se"
        )
    return points[name]


class _RecordBlock:
    """A detached group of layout records used by composition objects."""

    def __init__(
        self, shapes=None, ports=None, instances=None, devices=None, layers=None
    ):
        self.shapes = list(shapes or [])
        self.ports = list(ports or [])
        self.instances = list(instances or [])
        self.devices = list(devices or [])
        self.layers = dict(layers or {})
        self.handles = []

    @property
    def bounds(self):
        bounds = _bounds_of_records(self.shapes, self.ports)
        instance_boxes = [
            _instance_world_bounds(instance) for instance in self.instances
        ]
        return _union_bounds([bounds] + instance_boxes)

    @property
    def width(self):
        bounds = self.bounds
        return 0 if bounds is None else bounds[2] - bounds[0]

    @property
    def height(self):
        bounds = self.bounds
        return 0 if bounds is None else bounds[3] - bounds[1]

    @property
    def is_empty(self):
        return (
            not self.shapes
            and not self.ports
            and not self.instances
            and not self.devices
        )

    def clone(self):
        return _RecordBlock(
            copy.deepcopy(self.shapes),
            copy.deepcopy(self.ports),
            copy.deepcopy(self.instances),
            copy.deepcopy(self.devices),
            copy.deepcopy(self.layers),
        )

    def translate(self, dx, dy):
        _translate_block(self, dx, dy)
        return self

    def mirror(self, axis="y"):
        _mirror_block(self, axis)
        return self

    def append(self, other):
        self.shapes.extend(other.shapes)
        self.ports.extend(other.ports)
        self.instances.extend(other.instances)
        self.devices.extend(other.devices)
        self.handles.extend(other.handles)
        for name, spec in other.layers.items():
            self.layers.setdefault(name, dict(spec))
        return self


def _block_from_layout(layout, deep=False):
    selector = copy.deepcopy if deep else (lambda value: value)
    return _RecordBlock(
        selector(list(layout.shapes)),
        selector(list(layout.ports)),
        selector(list(layout.instances)),
        selector(list(layout.devices)),
        selector(dict(layout.layers)),
    )


def _remove_identity(records, value):
    """Remove ``value`` from ``records`` by identity, not by dict equality.

    Two devices created at the same local position produce equal shape dicts.
    ``list.remove`` compares by value, so it could evict another device's
    record and leave this one behind in the parent builder (stale geometry
    with no matching ports after composition).
    """
    for index, existing in enumerate(records):
        if existing is value:
            del records[index]
            return True
    return False


def _detach_handle_records(item):
    """Remove a concrete handle's records from its owning builder."""
    owner = getattr(item, "_layout", None)
    if isinstance(item, Shape):
        if owner is not None:
            _remove_identity(owner.shapes, item._record)
    elif isinstance(item, DevicePlacement):
        if owner is not None:
            for shape in item.shapes:
                _remove_identity(owner.shapes, shape._record)
            for record in getattr(item, "_port_records", []):
                _remove_identity(owner.ports, record)
            device_record = getattr(item, "_device_record", None)
            if device_record is not None:
                _remove_identity(owner.devices, device_record)
            if item in getattr(owner, "_device_handles", []):
                owner._device_handles.remove(item)
    elif isinstance(item, Route):
        if owner is not None:
            for shape in item.shapes:
                _remove_identity(owner.shapes, shape._record)
            if item in getattr(owner, "_route_handles", []):
                owner._route_handles.remove(item)
    elif isinstance(item, LayoutInstancePlacement):
        if owner is not None:
            _remove_identity(owner.instances, item._record)


def _copy_handle_block(item, detach=False):
    """Return a detached copy of everything an item contributes."""
    if item is None:
        return _RecordBlock()
    if isinstance(item, (list, tuple)):
        block = _RecordBlock()
        for entry in item:
            block.append(_copy_handle_block(entry, detach=detach))
        return block
    if detach and isinstance(
        item, (Shape, DevicePlacement, Route, LayoutInstancePlacement)
    ):
        _detach_handle_records(item)
    clone = (lambda value: value) if detach else copy.deepcopy
    if isinstance(item, Shape):
        block = _RecordBlock([clone(item._record)])
        if detach:
            block.handles.append(item)
        return block
    if isinstance(item, DevicePlacement):
        block = _RecordBlock()
        block.shapes = [clone(shape._record) for shape in item.shapes]
        block.ports = clone(list(getattr(item, "_port_records", [])))
        if not block.ports:
            for pin, rectangle in item.ports.items():
                info = item.port_info.get(pin, {})
                block.ports.append(
                    {
                        "name": str(pin),
                        "layer": info.get("layer", ""),
                        "rectangle": {
                            "x": rectangle[0],
                            "y": rectangle[1],
                            "width": rectangle[2],
                            "height": rectangle[3],
                        },
                        "direction": info.get("direction", "inout"),
                        "side": info.get("side", "left"),
                        "net": info.get("net"),
                        "label": str(pin),
                        "visible": True,
                        "locked": False,
                        "internal": True,
                    }
                )
        if isinstance(getattr(item, "_device_record", None), dict):
            block.devices = [clone(item._device_record)]
        else:
            bounds = item.bounds or {"x": 0, "y": 0, "width": 1, "height": 1}
            block.devices = [
                {
                    "name": item.name,
                    "device": item.device,
                    "model": item.model,
                    "type": item.model or item.device,
                    "parameters": [
                        {"name": str(name), "value": str(value)}
                        for name, value in item.parameters.items()
                    ],
                    "bounds": copy.deepcopy(bounds),
                    "ports": [
                        {
                            "name": str(pin),
                            "net": item.port_info.get(pin, {}).get("net"),
                            "layer": item.port_info.get(pin, {}).get("layer", ""),
                            "side": item.port_info.get(pin, {}).get("side", "left"),
                            "direction": item.port_info.get(pin, {}).get(
                                "direction", "inout"
                            ),
                            "rectangle": {
                                "x": rectangle[0],
                                "y": rectangle[1],
                                "width": rectangle[2],
                                "height": rectangle[3],
                            },
                        }
                        for pin, rectangle in item.ports.items()
                    ],
                }
            ]
        if detach:
            block.handles.append(item)
        return block
    if isinstance(item, Route):
        block = _RecordBlock([clone(shape._record) for shape in item.shapes])
        if detach:
            block.handles.append(item)
        return block
    if isinstance(item, LayoutInstancePlacement):
        block = _RecordBlock(instances=[clone(item._record)])
        if detach:
            block.handles.append(item)
        return block
    if isinstance(item, Layout):
        return _block_from_layout(item, deep=True)
    if isinstance(item, _Container):
        return _RecordBlock(
            copy.deepcopy(item.shapes),
            copy.deepcopy(item.ports),
            copy.deepcopy(item.instances),
            copy.deepcopy(item.devices),
            copy.deepcopy(item.layers),
        )
    raise TypeError(
        "layout composition accepts shapes, devices, routes, groups, rows, "
        "columns, grids, stacks, repeats, spacers, and stretches"
    )


def _translate_route_metadata(route, dx, dy):
    route.points = [(x + dx, y + dy) for x, y in route.points]
    for segment in route.segments:
        segment["start"] = (segment["start"][0] + dx, segment["start"][1] + dy)
        segment["end"] = (segment["end"][0] + dx, segment["end"][1] + dy)
    for via in route.vias:
        via["x"] += dx
        via["y"] += dy


def _mirror_route_metadata(route, flip_x, flip_y, low_x, high_x, low_y, high_y):
    route.points = [
        (
            low_x + high_x - x if flip_x else x,
            low_y + high_y - y if flip_y else y,
        )
        for x, y in route.points
    ]
    for segment in route.segments:
        start = segment["start"]
        end = segment["end"]
        segment["start"] = (
            low_x + high_x - start[0] if flip_x else start[0],
            low_y + high_y - start[1] if flip_y else start[1],
        )
        segment["end"] = (
            low_x + high_x - end[0] if flip_x else end[0],
            low_y + high_y - end[1] if flip_y else end[1],
        )
    for via in route.vias:
        if flip_x:
            via["x"] = low_x + high_x - via["x"]
        if flip_y:
            via["y"] = low_y + high_y - via["y"]


def _translate_block(block, dx, dy):
    for record in block.shapes:
        _translate_record(record, dx, dy)
    for port in block.ports:
        rectangle = port.get("rectangle")
        if isinstance(rectangle, dict):
            rectangle["x"] += dx
            rectangle["y"] += dy
        elif isinstance(rectangle, (list, tuple)) and len(rectangle) == 4:
            port["rectangle"] = (
                rectangle[0] + dx,
                rectangle[1] + dy,
                rectangle[2],
                rectangle[3],
            )
    for instance in block.instances:
        instance["x"] += dx
        instance["y"] += dy
    for device in block.devices:
        bounds = device.get("bounds")
        if isinstance(bounds, dict):
            bounds["x"] += dx
            bounds["y"] += dy
        for port in device.get("ports", []):
            rectangle = port.get("rectangle")
            if isinstance(rectangle, dict):
                rectangle["x"] += dx
                rectangle["y"] += dy
    for handle in block.handles:
        if isinstance(handle, DevicePlacement):
            handle.refresh()
        elif isinstance(handle, Route):
            _translate_route_metadata(handle, dx, dy)
    return block


def _mirror_block(block, axis="y"):
    bounds = block.bounds
    if bounds is None:
        return block
    flip_x, flip_y = _axis_flags(axis)
    low_x, low_y, high_x, high_y = bounds
    for record in block.shapes:
        _mirror_record(record, flip_x, flip_y, low_x, high_x, low_y, high_y)
    _mirror_ports(block.ports, flip_x, flip_y, low_x, high_x, low_y, high_y)
    for instance in block.instances:
        if flip_x:
            instance["x"] = low_x + high_x - instance["x"]
        if flip_y:
            instance["y"] = low_y + high_y - instance["y"]
        instance["orientation"] = _mirror_orientation(
            instance.get("orientation", "r0"), flip_x, flip_y
        )
    for device in block.devices:
        bounds = device.get("bounds")
        if not isinstance(bounds, dict):
            continue
        if flip_x:
            bounds["x"] = low_x + high_x - bounds["x"] - bounds.get("width", 0)
        if flip_y:
            bounds["y"] = low_y + high_y - bounds["y"] - bounds.get("height", 0)
        _mirror_ports(
            device.get("ports", []),
            flip_x,
            flip_y,
            low_x,
            high_x,
            low_y,
            high_y,
        )
    for handle in block.handles:
        if isinstance(handle, DevicePlacement):
            handle.refresh()
        elif isinstance(handle, Route):
            _mirror_route_metadata(handle, flip_x, flip_y, low_x, high_x, low_y, high_y)
    return block


def _fragment_bounds(fragment):
    value = fragment.bounds
    return value() if callable(value) else value


def _handle_bounds(item):
    if item is None:
        return None
    if isinstance(item, (Shape, DevicePlacement, Route, LayoutInstancePlacement)):
        bounds = item.bounds
        if isinstance(bounds, dict):
            return (
                bounds.get("x", 0),
                bounds.get("y", 0),
                bounds.get("x", 0) + bounds.get("width", 0),
                bounds.get("y", 0) + bounds.get("height", 0),
            )
        return bounds
    if isinstance(item, Layout):
        return _fragment_bounds(item)
    if isinstance(item, _Container):
        return item.bounds
    if isinstance(item, _Spacer):
        return (0, 0, item.width, item.height)
    if isinstance(item, (list, tuple)):
        return _union_bounds([_handle_bounds(entry) for entry in item])
    raise TypeError(f"cannot measure {type(item).__name__}")


def _handle_ports(item):
    if isinstance(item, (DevicePlacement, LayoutInstancePlacement)):
        return dict(item.ports)
    if isinstance(item, (Layout, _Container)):
        result = {}
        for port in item.ports:
            rectangle = port.get("rectangle", {})
            result[str(port.get("name", ""))] = (
                rectangle.get("x", 0),
                rectangle.get("y", 0),
                rectangle.get("width", 0),
                rectangle.get("height", 0),
            )
        return result
    return {}


def _translate_handle(item, dx, dy):
    if isinstance(
        item,
        (Shape, DevicePlacement, Route, LayoutInstancePlacement, Layout, _Container),
    ):
        item.move(dx, dy)
        return
    if isinstance(item, _Spacer):
        return
    if isinstance(item, (list, tuple)):
        for entry in item:
            _translate_handle(entry, dx, dy)
        return
    raise TypeError(f"cannot move {type(item).__name__}")


def _mirror_handle(item, axis="y"):
    if isinstance(
        item,
        (Shape, DevicePlacement, Route, LayoutInstancePlacement, Layout, _Container),
    ):
        item.mirror(axis)
        return
    if isinstance(item, _Spacer):
        return
    if isinstance(item, (list, tuple)):
        for entry in item:
            _mirror_handle(entry, axis)
        return
    raise TypeError(f"cannot mirror {type(item).__name__}")


def _merge_block_into_layout(layout, block):
    layout.shapes.extend(block.shapes)
    layout.ports.extend(block.ports)
    layout.instances.extend(block.instances)
    layout.devices.extend(block.devices)
    for name, spec in block.layers.items():
        current = layout.layers.setdefault(name, {"number": None, "datatype": None})
        if spec.get("number") is not None and current.get("number") is None:
            current["number"] = spec["number"]
        if spec.get("datatype") is not None and current.get("datatype") is None:
            current["datatype"] = spec["datatype"]


def _snap_value(value, pitch):
    pitch = max(1, int(pitch))
    return int(round(value / pitch)) * pitch


def _snap_block(block, pitch):
    pitch = max(1, int(pitch))
    for record in block.shapes:
        _snap_record(record, pitch)
    for port in block.ports:
        rectangle = port.get("rectangle")
        if isinstance(rectangle, dict):
            rectangle["x"] = _snap_value(rectangle["x"], pitch)
            rectangle["y"] = _snap_value(rectangle["y"], pitch)
            rectangle["width"] = max(pitch, _snap_value(rectangle["width"], pitch))
            rectangle["height"] = max(pitch, _snap_value(rectangle["height"], pitch))
    for instance in block.instances:
        instance["x"] = _snap_value(instance["x"], pitch)
        instance["y"] = _snap_value(instance["y"], pitch)
    for device in block.devices:
        bounds = device.get("bounds")
        if isinstance(bounds, dict):
            bounds["x"] = _snap_value(bounds["x"], pitch)
            bounds["y"] = _snap_value(bounds["y"], pitch)
            bounds["width"] = max(pitch, _snap_value(bounds["width"], pitch))
            bounds["height"] = max(pitch, _snap_value(bounds["height"], pitch))
        for port in device.get("ports", []):
            rectangle = port.get("rectangle")
            if isinstance(rectangle, dict):
                rectangle["x"] = _snap_value(rectangle["x"], pitch)
                rectangle["y"] = _snap_value(rectangle["y"], pitch)
                rectangle["width"] = max(pitch, _snap_value(rectangle["width"], pitch))
                rectangle["height"] = max(
                    pitch, _snap_value(rectangle["height"], pitch)
                )
    for handle in block.handles:
        if isinstance(handle, DevicePlacement):
            handle.refresh()
    return block


def _snap_record(record, pitch):
    if "rectangle" in record:
        rectangle = record["rectangle"]
        rectangle["x"] = _snap_value(rectangle["x"], pitch)
        rectangle["y"] = _snap_value(rectangle["y"], pitch)
        rectangle["width"] = max(pitch, _snap_value(rectangle["width"], pitch))
        rectangle["height"] = max(pitch, _snap_value(rectangle["height"], pitch))
    for key in ("polygon", "centerline"):
        for point in record.get(key, []):
            point["x"] = _snap_value(point["x"], pitch)
            point["y"] = _snap_value(point["y"], pitch)
    for hole in record.get("holes", []):
        for point in hole:
            point["x"] = _snap_value(point["x"], pitch)
            point["y"] = _snap_value(point["y"], pitch)
    if "boolean" in record:
        for operand in record["boolean"].get("operands", []):
            _snap_record(operand, pitch)


def _prefix_block(block, prefix):
    prefix = str(prefix)
    for record in block.shapes:
        if record.get("name"):
            record["name"] = f"{prefix}{record['name']}"
    for port in block.ports:
        if port.get("name"):
            port["name"] = f"{prefix}{port['name']}"
        if port.get("label"):
            port["label"] = f"{prefix}{port['label']}"
    for instance in block.instances:
        if instance.get("name"):
            instance["name"] = f"{prefix}{instance['name']}"
    for device in block.devices:
        old_name = str(device.get("name", ""))
        if old_name:
            device["name"] = f"{prefix}{old_name}"
        for port in device.get("ports", []):
            if port.get("name"):
                port["name"] = f"{prefix}{port['name']}"
    return block


def _suffix_block(block, suffix):
    suffix = str(suffix)
    for record in block.shapes:
        if record.get("name"):
            record["name"] = f"{record['name']}{suffix}"
    for port in block.ports:
        if port.get("name"):
            port["name"] = f"{port['name']}{suffix}"
        if port.get("label"):
            port["label"] = f"{port['label']}{suffix}"
    for device in block.devices:
        if device.get("name"):
            device["name"] = f"{device['name']}{suffix}"
    for instance in block.instances:
        if instance.get("name"):
            instance["name"] = f"{instance['name']}{suffix}"
    return block


def _snap_handle(item, pitch):
    if isinstance(item, Shape):
        _snap_record(item._record, pitch)
    elif isinstance(item, DevicePlacement):
        for shape in item.shapes:
            _snap_record(shape._record, pitch)
        for pin, rectangle in list(item.ports.items()):
            item.ports[pin] = (
                _snap_value(rectangle[0], pitch),
                _snap_value(rectangle[1], pitch),
                max(pitch, _snap_value(rectangle[2], pitch)),
                max(pitch, _snap_value(rectangle[3], pitch)),
            )
        for info in item.port_info.values():
            rectangle = info.get("rectangle")
            if rectangle:
                info["rectangle"] = (
                    _snap_value(rectangle[0], pitch),
                    _snap_value(rectangle[1], pitch),
                    max(pitch, _snap_value(rectangle[2], pitch)),
                    max(pitch, _snap_value(rectangle[3], pitch)),
                )
        if item.bounds:
            item.bounds["x"] = _snap_value(item.bounds["x"], pitch)
            item.bounds["y"] = _snap_value(item.bounds["y"], pitch)
            item.bounds["width"] = max(pitch, _snap_value(item.bounds["width"], pitch))
            item.bounds["height"] = max(
                pitch, _snap_value(item.bounds["height"], pitch)
            )
        for record in getattr(item, "_port_records", []):
            rectangle = record.get("rectangle")
            if isinstance(rectangle, dict):
                rectangle["x"] = _snap_value(rectangle["x"], pitch)
                rectangle["y"] = _snap_value(rectangle["y"], pitch)
                rectangle["width"] = max(pitch, _snap_value(rectangle["width"], pitch))
                rectangle["height"] = max(
                    pitch, _snap_value(rectangle["height"], pitch)
                )
        item._sync_device_record()
    elif isinstance(item, Route):
        for shape in item.shapes:
            _snap_record(shape._record, pitch)
        item.points = [
            (_snap_value(x, pitch), _snap_value(y, pitch)) for x, y in item.points
        ]
        item.vias = [
            dict(via, x=_snap_value(via["x"], pitch), y=_snap_value(via["y"], pitch))
            for via in item.vias
        ]
    elif isinstance(item, LayoutInstancePlacement):
        item.x = _snap_value(item.x, pitch)
        item.y = _snap_value(item.y, pitch)
    elif isinstance(item, Layout):
        for record in item.shapes:
            _snap_record(record, pitch)
        for port in item.ports:
            rectangle = port.get("rectangle")
            if isinstance(rectangle, dict):
                rectangle["x"] = _snap_value(rectangle["x"], pitch)
                rectangle["y"] = _snap_value(rectangle["y"], pitch)
                rectangle["width"] = max(pitch, _snap_value(rectangle["width"], pitch))
                rectangle["height"] = max(
                    pitch, _snap_value(rectangle["height"], pitch)
                )
        for instance in item.instances:
            instance["x"] = _snap_value(instance["x"], pitch)
            instance["y"] = _snap_value(instance["y"], pitch)
        for device in item.devices:
            bounds = device.get("bounds")
            if isinstance(bounds, dict):
                bounds["x"] = _snap_value(bounds["x"], pitch)
                bounds["y"] = _snap_value(bounds["y"], pitch)
                bounds["width"] = max(pitch, _snap_value(bounds["width"], pitch))
                bounds["height"] = max(pitch, _snap_value(bounds["height"], pitch))
    elif isinstance(item, _Container):
        _snap_block(item, pitch)
    elif isinstance(item, (list, tuple)):
        for entry in item:
            _snap_handle(entry, pitch)


def _route_rect(value):
    if isinstance(value, dict):
        if "rectangle" in value:
            value = value["rectangle"]
        if isinstance(value, dict) and all(
            key in value for key in ("x", "y", "width", "height")
        ):
            return (
                _coordinate(value["x"], "route rectangle x"),
                _coordinate(value["y"], "route rectangle y"),
                _coordinate(value["width"], "route rectangle width"),
                _coordinate(value["height"], "route rectangle height"),
            )
        return None
    if isinstance(value, (list, tuple)) and len(value) == 4:
        return (
            _coordinate(value[0], "route rectangle x"),
            _coordinate(value[1], "route rectangle y"),
            _coordinate(value[2], "route rectangle width"),
            _coordinate(value[3], "route rectangle height"),
        )
    return None


def _route_point(value):
    if isinstance(value, dict) and "x" in value and "y" in value:
        return (
            _coordinate(value["x"], "route point x"),
            _coordinate(value["y"], "route point y"),
        )
    if isinstance(value, (list, tuple)) and len(value) == 2:
        return (
            _coordinate(value[0], "route point x"),
            _coordinate(value[1], "route point y"),
        )
    return None


def _route_center(rectangle):
    x, y, width, height = rectangle
    return x + width // 2, y + height // 2


def _route_side_toward(point, target):
    dx = target[0] - point[0]
    dy = target[1] - point[1]
    if abs(dx) >= abs(dy):
        return "right" if dx >= 0 else "left"
    return "top" if dy >= 0 else "bottom"


def _route_escape(rectangle, side, distance):
    x, y, width, height = rectangle
    center_x, center_y = _route_center(rectangle)
    if side == "left":
        return x - distance, center_y
    if side == "right":
        return x + width + distance, center_y
    if side == "top":
        return center_x, y + height + distance
    if side == "bottom":
        return center_x, y - distance
    raise ValueError("route side must be left, right, top, or bottom")


def _route_record_bounds(record):
    rectangle = record.get("rectangle")
    if isinstance(rectangle, dict):
        return (
            int(rectangle.get("x", 0)),
            int(rectangle.get("y", 0)),
            int(rectangle.get("width", 0)),
            int(rectangle.get("height", 0)),
        )
    polygon = record.get("polygon")
    if isinstance(polygon, list) and polygon:
        xs = [int(point.get("x", 0)) for point in polygon]
        ys = [int(point.get("y", 0)) for point in polygon]
        return min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys)
    centerline = record.get("centerline")
    if isinstance(centerline, list) and centerline:
        width = int(record.get("width", 0))
        half = (width + 1) // 2
        xs = [int(point.get("x", 0)) for point in centerline]
        ys = [int(point.get("y", 0)) for point in centerline]
        return (
            min(xs) - half,
            min(ys) - half,
            max(xs) - min(xs) + 2 * half,
            max(ys) - min(ys) + 2 * half,
        )
    return None


def _route_simplify(points):
    result = []
    for point in points:
        if result and point == result[-1]:
            continue
        if len(result) >= 2:
            first = result[-2]
            second = result[-1]
            if first[0] == second[0] == point[0] or first[1] == second[1] == point[1]:
                result[-1] = point
                continue
        result.append(point)
    return result


def _connectivity_key(name):
    return str(name).split(".", 1)[0].strip().lower()


def _layer_purpose_rank(name):
    text = str(name)
    if "." not in text:
        return 0
    purpose = text.split(".", 1)[1].split(".", 1)[0].lower()
    if purpose == "drawing":
        return 0
    if purpose == "pin":
        return 1
    return 2


def _via_names(value):
    if isinstance(value, str):
        return [_connectivity_key(value)]
    if isinstance(value, (list, tuple)):
        return [_connectivity_key(item) for item in value if str(item).strip()]
    return []


def _metal_index(name):
    base = name.split(".", 1)[0].lower()
    match = re.fullmatch(r"(?:met|metal|m)(\d+)", base)
    return int(match.group(1)) if match else None


def _via_index(name):
    base = name.split(".", 1)[0].lower()
    match = re.fullmatch(r"(?:via|topvia)(\d*)", base)
    if not match:
        return None
    return int(match.group(1)) if match.group(1) else 1


def _route_rects_overlap(first, second):
    return (
        first[0] <= second[0] + second[2]
        and second[0] <= first[0] + first[2]
        and first[1] <= second[1] + second[3]
        and second[1] <= first[1] + first[3]
    )


def _route_inflate(obstacles, inflate):
    inflated = []
    for x, y, width, height in obstacles:
        if width <= 0 or height <= 0:
            continue
        inflated.append(
            (
                x - inflate,
                y - inflate,
                x + width + inflate,
                y + height + inflate,
            )
        )
    return inflated


def _route_inside_obstacle(x, y, inflated):
    return any(
        left < x < right and top < y < bottom for left, top, right, bottom in inflated
    )


def _route_segment_blocked(first, second, inflated):
    x0, y0 = first
    x1, y1 = second
    if y0 == y1:
        low, high = sorted((x0, x1))
        return any(
            top < y0 < bottom and high > left and low < right
            for left, top, right, bottom in inflated
        )
    if x0 == x1:
        low, high = sorted((y0, y1))
        return any(
            left < x0 < right and high > top and low < bottom
            for left, top, right, bottom in inflated
        )
    return True


def _route_opposite(side):
    return {
        "left": "right",
        "right": "left",
        "top": "bottom",
        "bottom": "top",
    }.get(side)


def _route_choose_side(rectangle, preferred, other_center, distance, inflated):
    center = _route_center(rectangle)
    candidates = []
    for side in (
        preferred,
        _route_opposite(preferred),
        "top",
        "bottom",
        "left",
        "right",
    ):
        if side and side not in candidates:
            candidates.append(side)
    for side in candidates:
        escape = _route_escape(rectangle, side, distance)
        if _route_inside_obstacle(*escape, inflated):
            continue
        if _route_segment_blocked(center, escape, inflated):
            continue
        return side, center, escape
    x, y, width, height = rectangle
    raise RuntimeError(
        "no clear escape from the port at ({}, {}) size {}x{}: every side is blocked by "
        "metal within the route clearance. Reduce the route width/clearance, move the "
        "device, or escape from a different port.".format(x, y, width, height)
    )


def _route_simplify_layers(points):
    result = []
    for point in points:
        if result and result[-1] == point:
            continue
        if len(result) >= 2:
            first = result[-2]
            second = result[-1]
            if first[2] == second[2] == point[2] and (
                first[0] == second[0] == point[0] or first[1] == second[1] == point[1]
            ):
                result[-1] = point
                continue
        result.append(point)
    return result


def _route_manhattan_layers(
    start,
    start_layer,
    end,
    end_layer,
    inflated_by_layer,
    via_options,
    via_cost,
    bend_penalty,
    margin,
):
    layer_count = len(inflated_by_layer)
    if not (0 <= start_layer < layer_count and 0 <= end_layer < layer_count):
        raise RuntimeError("route layer is not in the layer stack")

    def inside_layer(x, y, layer):
        return _route_inside_obstacle(x, y, inflated_by_layer[layer])

    if inside_layer(*start, start_layer):
        raise RuntimeError(
            "route start is inside an obstacle; increase escape or clearance"
        )
    if inside_layer(*end, end_layer):
        raise RuntimeError(
            "route end is inside an obstacle; increase escape or clearance"
        )

    xs = {start[0], end[0]}
    ys = {start[1], end[1]}
    for inflated in inflated_by_layer:
        for left, top, right, bottom in inflated:
            xs.update((left, right))
            ys.update((top, bottom))
    xs.update((min(xs) - margin, max(xs) + margin))
    ys.update((min(ys) - margin, max(ys) + margin))
    xs = sorted(xs)
    ys = sorted(ys)
    x_index = {value: index for index, value in enumerate(xs)}
    y_index = {value: index for index, value in enumerate(ys)}
    start_index = (x_index[start[0]], y_index[start[1]])
    goal_index = (x_index[end[0]], y_index[end[1]])

    def heuristic(ix, iy, layer):
        return (
            abs(xs[ix] - end[0])
            + abs(ys[iy] - end[1])
            + abs(layer - end_layer) * via_cost
        )

    start_state = (start_index[0], start_index[1], start_layer, 0)
    candidates = (
        (1, 0, 1),
        (-1, 0, 1),
        (0, 1, 2),
        (0, -1, 2),
    )
    best = {start_state: 0}
    parents = {}
    heap = [
        (
            heuristic(start_index[0], start_index[1], start_layer),
            0,
            start_index[0],
            start_index[1],
            start_layer,
            0,
        )
    ]
    goal_state = None
    while heap:
        _, cost, ix, iy, layer, direction = heapq.heappop(heap)
        state = (ix, iy, layer, direction)
        if cost != best.get(state):
            continue
        if (ix, iy, layer) == (goal_index[0], goal_index[1], end_layer):
            goal_state = state
            break
        for dx, dy, new_direction in candidates:
            nx, ny = ix + dx, iy + dy
            if not (0 <= nx < len(xs) and 0 <= ny < len(ys)):
                continue
            if inside_layer(xs[nx], ys[ny], layer):
                continue
            if _route_segment_blocked(
                (xs[ix], ys[iy]), (xs[nx], ys[ny]), inflated_by_layer[layer]
            ):
                continue
            step = abs(xs[nx] - xs[ix]) + abs(ys[ny] - ys[iy])
            turn = 0 if direction == 0 or direction == new_direction else bend_penalty
            new_cost = cost + step + turn
            new_state = (nx, ny, layer, new_direction)
            if new_cost >= best.get(new_state, math.inf):
                continue
            best[new_state] = new_cost
            parents[new_state] = state
            heapq.heappush(
                heap,
                (
                    new_cost + heuristic(nx, ny, layer),
                    new_cost,
                    nx,
                    ny,
                    layer,
                    new_direction,
                ),
            )
        for new_layer in (layer - 1, layer + 1):
            if not (0 <= new_layer < layer_count):
                continue
            key = (min(layer, new_layer), max(layer, new_layer))
            option = via_options.get(key)
            if option is None:
                continue
            if not option["legal"](xs[ix], ys[iy]):
                continue
            new_cost = cost + option.get("cost", via_cost)
            new_state = (ix, iy, new_layer, 0)
            if new_cost >= best.get(new_state, math.inf):
                continue
            best[new_state] = new_cost
            parents[new_state] = state
            heapq.heappush(
                heap,
                (
                    new_cost + heuristic(ix, iy, new_layer),
                    new_cost,
                    ix,
                    iy,
                    new_layer,
                    0,
                ),
            )

    if goal_state is None:
        raise RuntimeError(
            "no Manhattan route found; move the devices or adjust clearance"
        )
    path = []
    state = goal_state
    while state is not None:
        ix, iy, layer, _ = state
        path.append((xs[ix], ys[iy], layer))
        state = parents.get(state)
    path.reverse()
    simplified = []
    for point in path:
        if simplified and simplified[-1] == point:
            continue
        if len(simplified) >= 2:
            first = simplified[-2]
            second = simplified[-1]
            if first[2] == second[2] == point[2] and (
                first[0] == second[0] == point[0] or first[1] == second[1] == point[1]
            ):
                simplified[-1] = point
                continue
        simplified.append(point)
    return simplified


class Layer:
    """Handle returned by :meth:`Layout.layer`.

    The handle binds a layer name (and optionally its stream number and
    datatype) so geometry calls do not need to repeat the layer argument.
    ``rect`` and ``poly`` are aliases for :meth:`rectangle` and
    :meth:`polygon`.

    :ivar name: Layer name, including any purpose suffix such as ``.pin``.
    :ivar number: GDS layer number, or ``None`` when it is resolved later.
    :ivar datatype: GDS datatype, or ``None`` when it is resolved later.
    """

    def __init__(self, layout, name, number=None, datatype=None):
        self._layout = layout
        self.name = name
        self.number = number
        self.datatype = datatype

    def rectangle(self, x, y, width, height, name=None, net=None):
        """Draw a rectangle on this layer; see :meth:`Layout.rectangle`."""
        return self._layout.rectangle(
            self.name, x, y, width, height, name=name, net=net
        )

    rect = rectangle

    def polygon(self, points, holes=None, name=None, net=None):
        """Draw a polygon on this layer; see :meth:`Layout.polygon`."""
        return self._layout.polygon(self.name, points, holes=holes, name=name, net=net)

    poly = polygon

    def path(self, points, width, name=None, net=None):
        """Draw a path on this layer; see :meth:`Layout.path`."""
        return self._layout.path(self.name, points, width, name=name, net=net)

    def text(self, x, y, text, height=1000, width=None, name=None, net=None):
        """Draw text on this layer; see :meth:`Layout.text`."""
        return self._layout.text(
            self.name, x, y, text, height=height, width=width, name=name, net=net
        )


class Layout:
    """Builder exposed to procedural layout scripts as ``layout``.

    A script writes geometry by calling methods on this object. ICE executes
    the script in a temporary directory, reads the builder's output, validates
    it, and replaces the read-only generated child of the procedural layout.
    Coordinates are integer database units by default; use :meth:`nm`,
    :meth:`um`, :meth:`db`, or :meth:`snap` for physical values.

    :ivar pdk: Active PDK package id, or ``None`` for a generic layout.
    :ivar pdk_root: Installed package root used for device and connectivity
        lookups, or ``None`` when no package is active.
    :ivar db_unit_nm: Size of one database unit in nanometers.
    :ivar grid: Project manufacturing/snap grid in database units.
    :ivar params: Current parameter values keyed by parameter name.
    :ivar parameter_specs: Declared parameter metadata keyed by name.
    :ivar cells: Project layout-cell catalog keyed by cell name. Entries carry
        ``bounds`` and ``ports`` when ICE supplied the catalog.
    :ivar layers: Registered layer specs keyed by layer name.
    :ivar shapes: Emitted shape records in creation order.
    :ivar ports: Emitted port records in creation order.
    :ivar instances: Emitted child-instance records in creation order.
    :ivar devices: Planned-device metadata used by LVS and extraction.
    :ivar dummy_fill_requests: Queued PDK dummy-fill requests.
    """

    def __init__(
        self,
        pdk=None,
        pdk_root=None,
        db_unit_nm=1.0,
        grid=1,
        parameters=None,
        cells=None,
    ):
        self.pdk = pdk
        self.pdk_root = pdk_root
        self.db_unit_nm = float(db_unit_nm)
        self.grid = _GridValue(_coordinate(grid, "grid"), self)
        self.params = {
            str(name): "" if value is None else str(value)
            for name, value in (parameters or {}).items()
        }
        self.cells = {}
        for entry in cells or []:
            if not isinstance(entry, dict):
                continue
            name = str(entry.get("name", "")).strip()
            if name:
                self.cells[name] = entry
        self.parameter_specs = {}
        self.layers = {}
        self.shapes = []
        self.ports = []
        self.instances = []
        self.devices = []
        self.dummy_fill_requests = []
        self._device_library = None
        self._device_layers = {}
        self._connectivity = self._load_connectivity()
        self._device_geometry_cache = {}
        self._device_count = 0
        self._route_count = 0
        self._generators = {}
        self._generator_metadata = {}
        self._used_generators = {}
        self._constraint_records = []
        self._builtin_generators = None
        self._device_handles = []
        self._route_handles = []
        self._layout_state = {
            "plan": None,
            "compaction_passed": False,
            "compaction_quality": None,
            "review": None,
            "composition": {
                "group": 0,
                "row": 0,
                "column": 0,
                "grid": 0,
                "stack": 0,
                "repeat": 0,
                "spacer": 0,
                "stretch": 0,
                "constraint": 0,
                "generator": 0,
                "add": 0,
            },
            "manual_moves": 0,
        }

    def _note_composition(self, kind):
        stats = self._layout_state["composition"]
        stats[kind] = stats.get(kind, 0) + 1

    def _layer_spec(self, name, number=None, datatype=None):
        if not isinstance(name, str) or not name.strip():
            raise ValueError("Layer name must be a non-empty string")
        name = name.strip()
        if number is not None:
            number = _coordinate(number, f"Layer {name} number")
            if number < 0:
                raise ValueError(f"Layer {name} number must not be negative")
        if datatype is not None:
            datatype = _coordinate(datatype, f"Layer {name} datatype")
            if datatype < 0:
                raise ValueError(f"Layer {name} datatype must not be negative")
        current = self.layers.get(name, {"number": None, "datatype": None})
        if number is not None or name not in self.layers:
            current["number"] = number
        if datatype is not None or name not in self.layers:
            current["datatype"] = datatype
        self.layers[name] = current
        return name, current["number"], current["datatype"]

    def layer(self, name, number=None, datatype=None):
        """Register or resolve a layer and return a reusable handle.

        When ``number``/``datatype`` are omitted, ICE resolves the layer from
        the active PDK package or the project's existing layers. Explicit
        values win when no matching PDK/project layer exists. A purpose suffix
        such as ``Metal1.pin`` is preserved, so pin layers can carry their own
        datatype.

        :param name: Non-empty layer name.
        :param number: Optional GDS layer number.
        :param datatype: Optional GDS datatype.
        :returns: A :class:`Layer` handle bound to the resolved name.
        :raises ValueError: If the name is empty or the stream values are
            negative.
        """
        name, number, datatype = self._layer_spec(name, number, datatype)
        return Layer(self, name, number, datatype)

    def _layer_name(self, layer):
        if isinstance(layer, Layer):
            name = layer.name
        elif isinstance(layer, str):
            name = layer.strip()
        else:
            raise ValueError("Layer must be a layer name or a Layout.layer() handle")
        if not name:
            raise ValueError("Layer name must be a non-empty string")
        if name not in self.layers:
            self.layers[name] = {"number": None, "datatype": None}
        return name

    def _add_shape(self, record):
        self.shapes.append(record)
        return Shape(self, record)

    def rectangle(self, layer, x, y, width, height, name=None, net=None):
        """Draw an axis-aligned filled rectangle.

        :param layer: Layer name or :class:`Layer` handle.
        :param x: Left edge in database units.
        :param y: Bottom edge in database units.
        :param width: Positive width in database units.
        :param height: Positive height in database units.
        :param name: Optional shape name.
        :param net: Optional net name for connectivity checks and routing.
        :returns: The created :class:`Shape`.
        """
        record = {
            "layer": self._layer_name(layer),
            "rectangle": {
                "x": _coordinate(x, "rectangle x"),
                "y": _coordinate(y, "rectangle y"),
                "width": _positive(width, "rectangle width"),
                "height": _positive(height, "rectangle height"),
            },
        }
        shape = self._add_shape(record)
        if name is not None:
            shape.name = name
        if net is not None:
            shape.net = net
        return shape

    rect = rectangle

    def polygon(self, layer, points, holes=None, name=None, net=None):
        """Draw a filled polygon, optionally with hole contours.

        Points may be ``(x, y)`` tuples or mappings with ``x`` and ``y`` keys.
        Holes use the same point format and must contain at least three
        points.

        :param layer: Layer name or :class:`Layer` handle.
        :param points: Outer contour with at least three points.
        :param holes: Optional list of hole contours.
        :param name: Optional shape name.
        :param net: Optional net name for connectivity checks and routing.
        :returns: The created :class:`Shape`.
        :raises ValueError: If the contour has fewer than three points.
        """
        name_value = self._layer_name(layer)
        polygon = []
        for index, point in enumerate(points):
            x, y = _point(point, f"polygon point {index}")
            polygon.append({"x": x, "y": y})
        if len(polygon) < 3:
            raise ValueError("polygon must contain at least three points")
        record = {"layer": name_value, "polygon": polygon}
        if holes:
            record["holes"] = []
            for hole_index, hole in enumerate(holes):
                contour = []
                for point_index, point in enumerate(hole):
                    x, y = _point(point, f"hole {hole_index} point {point_index}")
                    contour.append({"x": x, "y": y})
                if len(contour) < 3:
                    raise ValueError("polygon holes must contain at least three points")
                record["holes"].append(contour)
        shape = self._add_shape(record)
        if name is not None:
            shape.name = name
        if net is not None:
            shape.net = net
        return shape

    poly = polygon

    def _boolean_operand(self, operand, layer):
        if isinstance(operand, Shape):
            record = copy.deepcopy(operand._record)
            if "centerline" in record or "text" in record:
                raise ValueError("Boolean operands must be rectangles or polygons")
            if (
                "boolean" not in record
                and "polygon" not in record
                and "rectangle" not in record
            ):
                raise ValueError("Boolean operand has no filled geometry")
            operand._record["boolean_operand"] = True
            record["boolean_operand"] = True
            return record
        if not isinstance(operand, (list, tuple)):
            raise ValueError("Boolean operands must be shapes or point lists")
        if len(operand) == 4 and all(
            isinstance(value, (int, float)) for value in operand
        ):
            x, y, width, height = operand
            return {
                "layer": layer,
                "rectangle": {
                    "x": _coordinate(x, "boolean rectangle x"),
                    "y": _coordinate(y, "boolean rectangle y"),
                    "width": _positive(width, "boolean rectangle width"),
                    "height": _positive(height, "boolean rectangle height"),
                },
            }
        polygon = []
        for index, point in enumerate(operand):
            x, y = _point(point, f"boolean point {index}")
            polygon.append({"x": x, "y": y})
        if len(polygon) < 3:
            raise ValueError("Boolean polygon operands need at least three points")
        return {"layer": layer, "polygon": polygon}

    def boolean(self, operation, layer, *operands, name=None, net=None):
        """Combine two or more rectangles/polygons with a boolean operation.

        Supported operations are ``union``, ``intersection``, ``difference``
        and ``xor``. Operands can be :class:`Shape` handles, ``(x, y, width,
        height)`` rectangles, or point lists. The result is emitted on
        ``layer`` and the operand shapes are consumed by the operation.

        :param operation: ``union``, ``intersection``, ``difference``, or
            ``xor`` (aliases such as ``or``, ``and``, ``subtract``, and
            ``minus`` are accepted).
        :param layer: Output layer name or :class:`Layer` handle.
        :param operands: Two or more shapes or geometry operands.
        :param name: Optional shape name.
        :param net: Optional net name.
        :returns: The created :class:`Shape`.
        """

        operation = str(operation).strip().lower()
        aliases = {
            "union": "union",
            "or": "union",
            "intersection": "intersection",
            "and": "intersection",
            "difference": "difference",
            "subtract": "difference",
            "minus": "difference",
            "xor": "xor",
        }
        if operation not in aliases:
            raise ValueError(
                "Boolean operation must be union, intersection, difference, or xor"
            )
        operation = aliases[operation]
        if len(operands) < 2:
            raise ValueError("Boolean operations need at least two operands")
        layer_name = self._layer_name(layer)
        records = [self._boolean_operand(operand, layer_name) for operand in operands]
        shape = self._add_shape(
            {
                "layer": layer_name,
                "boolean": {"operation": operation, "operands": records},
            }
        )
        if name is not None:
            shape.name = name
        if net is not None:
            shape.net = net
        return shape

    def union(self, layer, *operands, name=None, net=None):
        """Alias for ``boolean("union", ...)``."""
        return self.boolean("union", layer, *operands, name=name, net=net)

    def intersection(self, layer, *operands, name=None, net=None):
        """Alias for ``boolean("intersection", ...)``."""
        return self.boolean("intersection", layer, *operands, name=name, net=net)

    def subtract(self, layer, *operands, name=None, net=None):
        """Alias for ``boolean("difference", ...)``."""
        return self.boolean("difference", layer, *operands, name=name, net=net)

    def xor(self, layer, *operands, name=None, net=None):
        """Alias for ``boolean("xor", ...)``."""
        return self.boolean("xor", layer, *operands, name=name, net=net)

    def path(self, layer, points, width, name=None, net=None):
        """Draw a stroked polyline as a path shape.

        The path stores its centerline and width. ICE expands it to polygons
        for display and for :meth:`iter_polygons`.

        :param layer: Layer name or :class:`Layer` handle.
        :param points: Two or more centerline ``(x, y)`` points.
        :param width: Positive path width in database units.
        :param name: Optional shape name.
        :param net: Optional net name for connectivity checks and routing.
        :returns: The created :class:`Shape`.
        """
        name_value = self._layer_name(layer)
        centerline = []
        for index, point in enumerate(points):
            x, y = _point(point, f"path point {index}")
            centerline.append({"x": x, "y": y})
        if len(centerline) < 2:
            raise ValueError("path must contain at least two points")
        shape = self._add_shape(
            {
                "layer": name_value,
                "centerline": centerline,
                "width": _positive(width, "path width"),
            }
        )
        if name is not None:
            shape.name = name
        if net is not None:
            shape.net = net
        return shape

    def text(self, layer, x, y, text, height=1000, width=None, name=None, net=None):
        """Place a text label on a layer.

        Text is represented as a bounding rectangle with text metadata. When
        ``width`` is omitted, a readable width is estimated from the label
        length and text height.

        :param layer: Layer name or :class:`Layer` handle.
        :param x: Left edge in database units.
        :param y: Bottom edge in database units.
        :param text: Non-empty label.
        :param height: Text height in database units (default 1000).
        :param width: Optional explicit text width in database units.
        :param name: Optional shape name.
        :param net: Optional net name.
        :returns: The created text :class:`Shape`.
        """
        if not isinstance(text, str) or not text:
            raise ValueError("Text must be a non-empty string")
        height = _positive(height, "text height")
        if width is None:
            width = max(1, int(round(len(text) * height * 0.6)))
        else:
            width = _positive(width, "text width")
        record = {
            "layer": self._layer_name(layer),
            "rectangle": {
                "x": _coordinate(x, "text x"),
                "y": _coordinate(y, "text y"),
                "width": width,
                "height": height,
            },
            "text": text,
        }
        shape = self._add_shape(record)
        if name is not None:
            shape.name = name
        if net is not None:
            shape.net = net
        return shape

    def via(
        self, layers, x, y, size=1000, width=None, height=None, name=None, net=None
    ):
        """Draw one rectangle on each of ``layers`` to form a via stack.

        This is a geometry convenience helper; it does not emit router via
        metadata. Use :meth:`route` or :meth:`connect` when the via must be
        included in an autorouted connection.

        :param layers: Layer name or list of layer names, ordered bottom-up.
        :param x: Left edge of the via cut in database units.
        :param y: Bottom edge of the via cut in database units.
        :param size: Default square size in database units.
        :param width: Optional width override.
        :param height: Optional height override.
        :param name: Optional name prefix; each layer gets ``name_<index>``.
        :param net: Optional net name applied to every shape.
        :returns: List of created :class:`Shape` handles.
        """
        if isinstance(layers, str):
            layers = [layers]
        elif layers is None:
            raise ValueError("via layers must not be empty")
        else:
            layers = [str(layer) for layer in layers]
        if not layers:
            raise ValueError("via layers must not be empty")
        width = size if width is None else width
        height = width if height is None else height
        shapes = []
        for index, layer in enumerate(layers):
            shape = self.rectangle(
                layer,
                x,
                y,
                width,
                height,
                name=None if name is None else f"{name}_{index}",
                net=net,
            )
            shapes.append(shape)
        return shapes

    def array(self, cell, count, pitch, x=0, y=0, orientation="r0", name=None):
        """Place a rectangular array of copies of an existing layout cell.

        Instances are named ``<name>_<column>_<row>`` and reference ``cell``
        rather than copying its geometry. The target may be a static layout
        cell, another generated cell, an imported digital device, or a package
        fixed-device cell.

        :param cell: Layout cell name.
        :param count: ``(columns, rows)``, both positive.
        :param pitch: ``(x_pitch, y_pitch)`` in database units.
        :param x: Optional x offset in database units.
        :param y: Optional y offset in database units.
        :param orientation: Placement orientation (see :meth:`instance`).
        :param name: Optional instance-name prefix (default ``array``).
        :returns: List of placed instance names.
        """
        try:
            columns, rows = (int(count[0]), int(count[1]))
            pitch_x, pitch_y = (pitch[0], pitch[1])
        except (TypeError, ValueError, IndexError) as error:
            raise ValueError(
                "array count and pitch must be two-element sequences"
            ) from error
        if columns <= 0 or rows <= 0:
            raise ValueError("array count must be positive")
        if name is None:
            name = "array"
        placed = []
        for row in range(rows):
            for column in range(columns):
                instance_name = f"{name}_{column}_{row}"
                self.instance(
                    cell,
                    _coordinate(x, "array x")
                    + column * _coordinate(pitch_x, "array pitch x"),
                    _coordinate(y, "array y")
                    + row * _coordinate(pitch_y, "array pitch y"),
                    orientation=orientation,
                    name=instance_name,
                )
                placed.append(instance_name)
        return placed

    def port(
        self,
        name,
        layer,
        x,
        y,
        width,
        height,
        direction="inout",
        side="left",
        net=None,
        label=None,
        visible=True,
        locked=False,
    ):
        """Add an intrinsic port to the generated layout cell.

        Ports are part of the generated cell's interface and can be used for
        hierarchy wiring, connectivity checks, LVS, and routing.

        :param name: Non-empty port name.
        :param layer: Layer name or :class:`Layer` handle.
        :param x: Left edge in database units.
        :param y: Bottom edge in database units.
        :param width: Positive port width in database units.
        :param height: Positive port height in database units.
        :param direction: ``input``, ``output``, ``inout``, ``power``, or
            ``ground``.
        :param side: ``left``, ``right``, ``top``, or ``bottom``.
        :param net: Optional net name bound to the port.
        :param label: Optional display label; defaults to ``name``.
        :param visible: Whether the port is visible in the layout editor.
        :param locked: Whether the port is protected from editing.
        :returns: ``None``. The port is stored on the builder.
        """
        if not isinstance(name, str) or not name.strip():
            raise ValueError("Port name must be a non-empty string")
        if direction not in {"input", "output", "inout", "power", "ground"}:
            raise ValueError(
                "Port direction must be input, output, inout, power, or ground"
            )
        if side not in {"left", "right", "top", "bottom"}:
            raise ValueError("Port side must be left, right, top, or bottom")
        if net is not None and (not isinstance(net, str) or not net.strip()):
            raise ValueError("Port net must be a non-empty string or None")
        if label is None:
            label = name.strip()
        if not isinstance(label, str) or not label.strip():
            raise ValueError("Port label must be a non-empty string")
        self.ports.append(
            {
                "name": name.strip(),
                "layer": self._layer_name(layer),
                "rectangle": {
                    "x": _coordinate(x, "port x"),
                    "y": _coordinate(y, "port y"),
                    "width": _positive(width, "port width"),
                    "height": _positive(height, "port height"),
                },
                "direction": direction,
                "side": side,
                "net": net.strip() if isinstance(net, str) else None,
                "label": label.strip(),
                "visible": bool(visible),
                "locked": bool(locked),
            }
        )

    def pin(self, name):
        """Return a port rectangle ``(x, y, width, height)`` by name."""
        for port in self.ports:
            if str(port.get("name")) == str(name):
                rectangle = port["rectangle"]
                return (
                    rectangle["x"],
                    rectangle["y"],
                    rectangle["width"],
                    rectangle["height"],
                )
        raise KeyError(f"layout has no port '{name}'")

    def instance(self, cell, x, y, orientation="r0", name=None):
        """Place an instance of an existing layout cell.

        The instance references another cell in the project, including static
        layouts, other generated procedurals, imported digital devices, and
        package fixed devices. Geometry is not copied into the generated cell.
        The returned handle is already part of the builder and can be moved,
        mirrored, composed with :meth:`add`/row/column helpers, or used as a
        routing endpoint.

        Cell names are validated against the project cell catalog when ICE
        supplies one. Matching is case-insensitive and the canonical project
        name is stored in the instance.

        :param cell: Non-empty layout cell name.
        :param x: Placement x origin in database units.
        :param y: Placement y origin in database units.
        :param orientation: ``r0``, ``r90``, ``r180``, ``r270``, ``mx``,
            ``my``, ``m45``, or ``m135``.
        :param name: Optional instance name.
        :returns: A :class:`LayoutInstancePlacement` handle.
        """
        if not isinstance(cell, str) or not cell.strip():
            raise ValueError("Instance cell must be a non-empty layout cell name")
        orientations = {"r0", "r90", "r180", "r270", "mx", "my", "m45", "m135"}
        orientation = str(orientation)
        if orientation not in orientations:
            raise ValueError(f"Unsupported orientation: {orientation}")
        if name is None:
            name = f"instance_{len(self.instances) + 1}"
        if not isinstance(name, str) or not name.strip():
            raise ValueError("Instance name must be a non-empty string")
        requested = cell.strip()
        canonical = requested
        info = self.cells.get(requested) if self.cells else None
        if info is None and self.cells:
            lowered = requested.lower()
            for candidate_name, candidate in self.cells.items():
                if candidate_name.lower() == lowered:
                    canonical = candidate_name
                    info = candidate
                    break
        if info is None and self.cells:
            available = ", ".join(sorted(self.cells)[:20])
            if len(self.cells) > 20:
                available += ", ..."
            raise ValueError(
                f"Unknown layout cell '{requested}'. Available cells: {available}"
            )
        record = {
            "cell": canonical,
            "name": name.strip(),
            "x": _coordinate(x, "instance x"),
            "y": _coordinate(y, "instance y"),
            "orientation": orientation,
        }
        if isinstance(info, dict) and isinstance(info.get("bounds"), dict):
            record["cell_bounds"] = copy.deepcopy(info["bounds"])
        self.instances.append(record)
        return LayoutInstancePlacement(self, record, info)

    def parameter(self, name, default="", description=""):
        """Declare a user-editable parameter and return its current value.

        Parameter values are always strings. Use :meth:`um`, :meth:`nm`,
        :meth:`length`, or :meth:`int`/``float`` conversions in the script.
        Declared parameters appear in the GUI parameter form and can be
        changed with the CLI, ``icepy``, or MCP before regeneration.

        :param name: Non-empty parameter name.
        :param default: Value used when the project has no stored value.
        :param description: Human-readable description shown by the editor.
        :returns: The current value as a string.
        """
        if not isinstance(name, str) or not name.strip():
            raise ValueError("Parameter name must be a non-empty string")
        name = name.strip()
        value = self.params.get(name, "" if default is None else str(default))
        self.parameter_specs[name] = {
            "name": name,
            "value": value,
            "description": str(description or name),
        }
        return value

    def _load_device_library(self):
        if self._device_library is not None:
            return self._device_library
        if not self.pdk_root:
            raise RuntimeError(
                "Device placement requires an installed PDK package root"
            )
        root = Path(self.pdk_root)
        index_path = root / "layout" / "devices" / "index.json"
        if not index_path.is_file():
            pdk = self.pdk or "unknown"
            raise RuntimeError(f"PDK '{pdk}' does not provide {index_path}")
        try:
            library = json.loads(index_path.read_text())
        except (OSError, json.JSONDecodeError) as error:
            raise RuntimeError(
                f"Unable to read device library {index_path}: {error}"
            ) from error
        self._device_layers = {}
        for layer in library.get("layers", []):
            try:
                number = int(layer["number"])
                datatype = int(layer.get("datatype", 0))
            except (KeyError, TypeError, ValueError):
                continue
            self._device_layers[(number, datatype)] = str(
                layer.get("name", f"Layer {number}")
            )
        self._device_library = library
        return library

    def _device_layer_name(self, number, datatype):
        self._load_device_library()
        key = (int(number), int(datatype))
        name = self._device_layers.get(key)
        if name is None:
            raise RuntimeError(
                f"Device geometry uses stream {number}/{datatype}, which is not declared by the package device library"
            )
        return name

    def _device_cell(self, device_name):
        library = self._load_device_library()
        cells = library.get("cells", [])
        for cell in cells:
            if str(cell.get("name", "")).lower() == str(device_name).lower():
                return cell
        for cell in cells:
            if str(device_name).lower() in str(cell.get("name", "")).lower():
                return cell
        raise RuntimeError(f"Unknown layout device '{device_name}'")

    def _device_geometry(self, cell, parameters):
        generator = cell.get("generator")
        if not generator:
            layout_path = cell.get("layout")
            if not layout_path:
                raise RuntimeError(
                    f"Device '{cell.get('name', '')}' has no generator or layout file"
                )
            path = Path(self.pdk_root) / "layout" / "devices" / str(layout_path)
            try:
                document = json.loads(path.read_text())
                return document["device"]
            except (OSError, KeyError, json.JSONDecodeError) as error:
                raise RuntimeError(
                    f"Unable to read device geometry {path}: {error}"
                ) from error

        key = (
            str(generator.get("module", "")),
            str(generator.get("class", "")),
            tuple(sorted((str(k), str(v)) for k, v in parameters.items())),
        )
        cached = self._device_geometry_cache.get(key)
        if cached is not None:
            return cached
        generator_path = Path(self.pdk_root) / "layout" / "devices" / "generator.py"
        if not generator_path.is_file():
            raise RuntimeError(f"Device generator is missing: {generator_path}")
        if os.environ.get("ICE_ALLOW_PACKAGE_CODE", "1") == "0":
            raise RuntimeError(
                "Package device generators require explicit permission. "
                "Mark the package trusted or set ICE_ALLOW_PACKAGE_CODE=1."
            )
        with tempfile.TemporaryDirectory(prefix="ice-device-") as directory:
            params_path = Path(directory) / "params.json"
            output_path = Path(directory) / "geometry.json"
            params_path.write_text(json.dumps(parameters))
            command = [
                sys.executable,
                str(generator_path),
                "--pdk-root",
                str(self.pdk_root),
                "--module",
                str(generator.get("module", "")),
                "--class",
                str(generator.get("class", "")),
                "--params-file",
                str(params_path),
                "--output",
                str(output_path),
            ]
            try:
                completed = subprocess.run(
                    command,
                    cwd=self.pdk_root,
                    capture_output=True,
                    text=True,
                    check=False,
                )
            except OSError as error:
                raise RuntimeError(
                    f"Unable to run device generator: {error}"
                ) from error
            if completed.returncode != 0:
                details = (completed.stderr or completed.stdout or "").strip()
                raise RuntimeError(
                    f"Device generator for '{cell.get('name', '')}' failed: {details or completed.returncode}"
                )
            try:
                geometry = json.loads(output_path.read_text())
            except (OSError, json.JSONDecodeError) as error:
                raise RuntimeError(
                    f"Unable to read generated device geometry: {error}"
                ) from error
        self._device_geometry_cache[key] = geometry
        return geometry

    @staticmethod
    def _device_transform(x, y, orientation):
        if orientation == "r0":
            return x, y
        if orientation == "r90":
            return -y, x
        if orientation == "r180":
            return -x, -y
        if orientation == "r270":
            return y, -x
        if orientation == "mx":
            return x, -y
        if orientation == "my":
            return -x, y
        raise ValueError(f"Unsupported device orientation: {orientation}")

    @staticmethod
    def _device_side(side, orientation):
        if orientation == "r0":
            return side
        if orientation == "r90":
            return {
                "left": "bottom",
                "right": "top",
                "top": "left",
                "bottom": "right",
            }.get(side, side)
        if orientation == "r180":
            return {
                "left": "right",
                "right": "left",
                "top": "bottom",
                "bottom": "top",
            }.get(side, side)
        if orientation == "r270":
            return {
                "left": "top",
                "right": "bottom",
                "top": "right",
                "bottom": "left",
            }.get(side, side)
        if orientation == "mx":
            return {"top": "bottom", "bottom": "top"}.get(side, side)
        if orientation == "my":
            return {"left": "right", "right": "left"}.get(side, side)
        return side

    def device(
        self,
        device_name,
        x=0,
        y=0,
        orientation="r0",
        name=None,
        nets=None,
        **parameters,
    ):
        """Place a package layout device (PCell or fixed cell) into this layout.

        The device geometry is flattened into the generated layout, while a
        device record preserves its model, parameters, and terminal nets for
        LVS and extraction. Ports are prefixed with the instance name so
        multiple copies can coexist. ``nets`` maps device port names to layout
        net names, for example ``nets={"G": "GATE", "S": "VSS"}``.

        :param device_name: Device-library cell name; matching is
            case-insensitive and falls back to a substring match.
        :param x: Placement x origin in database units.
        :param y: Placement y origin in database units.
        :param orientation: ``r0``, ``r90``, ``r180``, ``r270``, ``mx``, or
            ``my``.
        :param name: Optional instance name; defaults to ``device_<n>``.
        :param nets: Optional mapping of port names to layout net names.
        :param parameters: Device parameters as keyword arguments, for
            example ``w="2u"`` or ``nf="2"``.
        :returns: A :class:`DevicePlacement` with absolute port rectangles.
        :raises RuntimeError: If the active package has no device library,
            the device cannot be found, or its generator fails.
        """
        cell = self._device_cell(device_name)
        orientation = str(orientation)
        if orientation not in {"r0", "r90", "r180", "r270", "mx", "my"}:
            raise ValueError(
                "Device orientation must be r0, r90, r180, r270, mx, or my"
            )
        defaults = {
            str(parameter.get("name", "")): str(parameter.get("value", ""))
            for parameter in cell.get("parameters", [])
        }
        values = dict(defaults)
        for key, value in parameters.items():
            values[str(key)] = "" if value is None else str(value)
        geometry = self._device_geometry(cell, values)
        self._device_count += 1
        instance_name = (
            str(name).strip() if name is not None else f"device_{self._device_count}"
        )
        if not instance_name:
            raise ValueError("Device instance name must not be empty")
        origin_x = _coordinate(x, "device x")
        origin_y = _coordinate(y, "device y")
        net_map = {}
        for pin, net in (nets or {}).items():
            if net is None or str(net).strip() == "":
                continue
            net_map[str(pin)] = str(net).strip()
        shapes = []
        shape_points = []
        for source in geometry.get("shapes", []):
            points = []
            for point in source.get("points", []):
                if isinstance(point, dict):
                    px, py = point.get("x", 0), point.get("y", 0)
                else:
                    try:
                        px, py = point
                    except (TypeError, ValueError) as error:
                        raise RuntimeError(
                            "Device shape point must be (x, y)"
                        ) from error
                tx, ty = self._device_transform(
                    _coordinate(px, "device point x"),
                    _coordinate(py, "device point y"),
                    orientation,
                )
                points.append({"x": tx + origin_x, "y": ty + origin_y})
            if len(points) < 3:
                continue
            layer_name = self._device_layer_name(
                source.get("layer", 0), source.get("datatype", 0)
            )
            record = {"layer": layer_name, "polygon": points}
            source_name = source.get("name", "")
            if source_name:
                record["name"] = f"{instance_name}.{source_name}"
            if source.get("net"):
                record["net"] = str(source["net"])
            shape_points.extend(points)
            shapes.append(self._add_shape(record))
        port_rectangles = {}
        port_records = []
        layout_port_records = []
        port_info = {}
        for source in geometry.get("ports", []):
            rect = source.get("rectangle", {})
            corners = []
            for px, py in (
                (rect.get("x", 0), rect.get("y", 0)),
                (rect.get("x", 0) + rect.get("width", 0), rect.get("y", 0)),
                (
                    rect.get("x", 0) + rect.get("width", 0),
                    rect.get("y", 0) + rect.get("height", 0),
                ),
                (rect.get("x", 0), rect.get("y", 0) + rect.get("height", 0)),
            ):
                tx, ty = self._device_transform(
                    _coordinate(px, "device port x"),
                    _coordinate(py, "device port y"),
                    orientation,
                )
                corners.append((tx, ty))
            min_x = min(point[0] for point in corners)
            min_y = min(point[1] for point in corners)
            max_x = max(point[0] for point in corners)
            max_y = max(point[1] for point in corners)
            pin = str(source.get("name", "P"))
            port_name = f"{instance_name}.{pin}"
            rectangle = {
                "x": min_x + origin_x,
                "y": min_y + origin_y,
                "width": max(max_x - min_x, 1),
                "height": max(max_y - min_y, 1),
            }
            net = net_map.get(pin)
            layer_name = self._device_layer_name(
                source.get("layer", 0), source.get("datatype", 0)
            )
            side = self._device_side(str(source.get("side", "left")), orientation)
            direction = str(source.get("direction", "inout"))
            layout_port = {
                "name": port_name,
                "layer": layer_name,
                "rectangle": rectangle,
                "direction": direction,
                "side": side,
                "net": net,
                "label": port_name,
                "visible": bool(source.get("visible", True)),
                "locked": bool(source.get("locked", False)),
                "internal": True,
            }
            self.ports.append(layout_port)
            layout_port_records.append(layout_port)
            port_rectangles[pin] = (
                rectangle["x"],
                rectangle["y"],
                rectangle["width"],
                rectangle["height"],
            )
            port_info[pin] = {
                "rectangle": port_rectangles[pin],
                "layer": layer_name,
                "side": side,
                "direction": direction,
                "net": net,
            }
            port_records.append(
                {
                    "name": pin,
                    "net": net,
                    "layer": layer_name,
                    "side": side,
                    "direction": direction,
                    "rectangle": dict(rectangle),
                }
            )
        # Propagate port nets to the device geometry that carries each port.
        # This lets the router treat other terminals as obstacles while
        # allowing a connection to travel along its own port metal.
        for shape in shapes:
            record = shape._record
            shape_layer = record.get("layer")
            bounds = _route_record_bounds(record)
            if not bounds:
                continue
            nets = set()
            for port in port_records:
                if not port.get("net") or port.get("layer") != shape_layer:
                    continue
                rectangle = _route_rect(port.get("rectangle"))
                if rectangle and _route_rects_overlap(bounds, rectangle):
                    nets.add(port["net"])
            if len(nets) == 1:
                shape.net = next(iter(nets))
        if shape_points:
            min_x = min(point["x"] for point in shape_points)
            min_y = min(point["y"] for point in shape_points)
            max_x = max(point["x"] for point in shape_points)
            max_y = max(point["y"] for point in shape_points)
            bounds = {
                "x": min_x,
                "y": min_y,
                "width": max(max_x - min_x, 1),
                "height": max(max_y - min_y, 1),
            }
        else:
            bounds = {"x": origin_x, "y": origin_y, "width": 1, "height": 1}
        model = str(values.get("model", "") or cell.get("model", ""))
        device_record = {
            "name": instance_name,
            "device": str(cell.get("name", device_name)),
            "model": model,
            "type": model or str(cell.get("name", device_name)),
            "parameters": [
                {"name": str(key), "value": str(value)} for key, value in values.items()
            ],
            "bounds": bounds,
            "ports": port_records,
            "port_info": port_info,
        }
        self.devices.append(device_record)
        placement = DevicePlacement(
            instance_name,
            str(cell.get("name", device_name)),
            model,
            dict(values),
            shapes,
            port_rectangles,
            port_info,
            bounds,
            self,
            device_record,
            layout_port_records,
        )
        self._device_handles.append(placement)
        return placement

    def _normalize_route_layers(self, layer, layers):
        if layers is None:
            if layer is None:
                raise ValueError("route requires layer or layers")
            return [self._layer_name(layer)]
        if layer is not None:
            raise ValueError("pass either layer or layers, not both")
        if isinstance(layers, (str, Layer)):
            layers = [layers]
        normalized = []
        for value in layers:
            name = self._layer_name(value)
            if name not in normalized:
                normalized.append(name)
        if not normalized:
            raise ValueError("layers must not be empty")
        return normalized

    def _normalize_via_map(self, via_layers):
        result = {}
        for key, value in (via_layers or {}).items():
            if isinstance(key, str):
                parts = [
                    part.strip()
                    for part in key.replace("->", "|").replace(",", "|").split("|")
                    if part.strip()
                ]
                if len(parts) != 2:
                    raise ValueError(
                        "via layer keys must name two layers, for example 'met1->met2'"
                    )
                lower, upper = parts
            elif isinstance(key, (list, tuple)) and len(key) == 2:
                lower, upper = key
            else:
                raise ValueError("via layer keys must be (lower, upper) pairs")
            lower = self._layer_name(lower)
            upper = self._layer_name(upper)
            if isinstance(value, (str, Layer)):
                cuts = [self._layer_name(value)]
            else:
                cuts = [self._layer_name(item) for item in value]
            if not cuts:
                raise ValueError("via layer mappings must not be empty")
            result[(lower, upper)] = cuts
            result[(upper, lower)] = cuts
        return result

    def _load_connectivity(self):
        if not self.pdk_root:
            return {}
        path = Path(self.pdk_root) / "technology" / "connectivity.json"
        if not path.is_file():
            return {}
        try:
            document = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError):
            return {}
        if (
            not isinstance(document, dict)
            or int(document.get("schema_version", 0)) != 1
        ):
            return {}
        layers = {}
        for entry in document.get("layers", []):
            if not isinstance(entry, dict):
                continue
            name = _connectivity_key(entry.get("name", ""))
            role = str(entry.get("role", "")).strip().lower()
            if name and role:
                layers[name] = role
        vias = []
        for entry in document.get("vias", []):
            if not isinstance(entry, dict):
                continue
            name = _connectivity_key(entry.get("name", ""))
            lower = _via_names(entry.get("lower"))
            upper = _via_names(entry.get("upper"))
            if not name or not lower or not upper:
                continue
            vias.append(
                {
                    "name": name,
                    "lower": lower,
                    "upper": upper,
                    "size": entry.get("size"),
                    "enclosure": entry.get("enclosure"),
                }
            )
        routing = document.get("routing") or {}
        stack = routing.get("stack") if isinstance(routing, dict) else None
        if not isinstance(stack, list):
            stack = document.get("routing_stack")
        routing_stack = (
            [str(name) for name in stack or [] if str(name).strip()]
            if isinstance(stack, list)
            else []
        )
        return {"layers": layers, "vias": vias, "routing_stack": routing_stack}

    def _declared_via(self, lower, upper):
        lower_key = _connectivity_key(lower)
        upper_key = _connectivity_key(upper)
        for via in self._connectivity.get("vias", []):
            if lower_key in via["lower"] and upper_key in via["upper"]:
                return via
            if lower_key in via["upper"] and upper_key in via["lower"]:
                return via
        return None

    def routing_stack(self):
        """Return the vertical routing stack declared by the PDK package.

        Names are resolved to the layer spellings registered by the device
        library when possible, so the returned list can be passed directly to
        :meth:`route` or :meth:`connect`. When the package does not declare a
        stack, conductor layers are inferred from names such as ``met1``,
        ``Metal2``, or ``m3``.

        :returns: Ordered list of layer names, bottom to top.
        """
        declared = self._connectivity.get("routing_stack") or []
        if not declared:
            available = self._available_layer_metadata()
            candidates = []
            for name in available:
                index = _metal_index(name)
                if index is not None:
                    candidates.append((index, name))
            candidates.sort()
            return [name for _, name in candidates]
        available_names = list(self._available_layer_metadata())
        result = []
        for base in declared:
            key = _connectivity_key(base)
            matches = [
                name for name in available_names if _connectivity_key(name) == key
            ]
            matches.sort(key=lambda name: (_layer_purpose_rank(name), name))
            result.append(matches[0] if matches else base)
        return result

    def _available_layer_metadata(self):
        available = {}
        for name, spec in self.layers.items():
            available[name] = (spec.get("number"), spec.get("datatype"))
        try:
            self._load_device_library()
        except Exception:
            pass
        for (number, datatype), name in self._device_layers.items():
            available.setdefault(name, (number, datatype))
        return available

    def _register_inferred_layer(self, name, available):
        if name in self.layers:
            return
        number, datatype = available.get(name, (None, None))
        self.layer(name, number, datatype)

    def _infer_via_layer(self, lower, upper):
        available = self._available_layer_metadata()
        declared = self._declared_via(lower, upper)
        if declared:
            candidates = [
                candidate
                for candidate in available
                if _connectivity_key(candidate) == declared["name"]
            ]
            if candidates:
                candidates.sort(key=lambda name: (_layer_purpose_rank(name), name))
                candidate = candidates[0]
                self._register_inferred_layer(candidate, available)
                return candidate
        lower_base = lower.split(".", 1)[0].lower()
        upper_base = upper.split(".", 1)[0].lower()
        special = {
            frozenset(("li1", "met1")): ("mcon.drawing", "mcon", "Mcon"),
            frozenset(("poly", "li1")): ("licon1.drawing", "licon1", "licon"),
        }
        for pair, candidates in special.items():
            if frozenset((lower_base, upper_base)) == pair:
                for candidate in candidates:
                    if candidate in available:
                        self._register_inferred_layer(candidate, available)
                        return candidate
        lower_index = _metal_index(lower)
        upper_index = _metal_index(upper)
        if (
            lower_index is not None
            and upper_index is not None
            and upper_index != lower_index + 1
        ):
            return None
        via_candidates = []
        for name, (number, datatype) in available.items():
            index = _via_index(name)
            if index is None:
                continue
            via_candidates.append((name, index, number, datatype))
        if lower_index is not None:
            exact = [
                candidate for candidate in via_candidates if candidate[1] == lower_index
            ]
            if exact:
                exact.sort(
                    key=lambda item: (
                        0 if "topvia" not in item[0].lower() else 1,
                        item[0],
                    )
                )
                name = exact[0][0]
                self._register_inferred_layer(name, available)
                return name
        return None

    @staticmethod
    def _route_extra_for_layer(extra, layer_name):
        if isinstance(extra, dict) and not all(
            key in extra for key in ("x", "y", "width", "height")
        ):
            return extra.get(layer_name, [])
        return extra

    def _device_port_for_rectangle(self, rectangle):
        for device in self.devices:
            for info in (device.get("port_info") or {}).values():
                if _route_rect(info.get("rectangle")) == rectangle:
                    return device, info
        for instance in self.instances:
            cell_info = self.cells.get(str(instance.get("cell", "")))
            ports = _instance_port_info(instance, cell_info)
            for info in ports.values():
                if _route_rect(info.get("rectangle")) != rectangle:
                    continue
                box = _instance_world_bounds(instance)
                owner = {
                    "name": instance.get("name", ""),
                    "bounds": {
                        "x": box[0],
                        "y": box[1],
                        "width": box[2] - box[0],
                        "height": box[3] - box[1],
                    },
                    "port_info": ports,
                }
                return owner, info
        return None, None

    def _device_bounds_for_rectangle(self, rectangle):
        for device in self.devices:
            for info in (device.get("port_info") or {}).values():
                if _route_rect(info.get("rectangle")) == rectangle:
                    return _route_rect(device.get("bounds"))
        return None

    def _route_obstacles(
        self,
        layer_name,
        net,
        include_devices,
        include_shapes,
        extra,
        exclude_device_bounds=None,
    ):
        obstacles = []
        excluded = []
        for value in exclude_device_bounds or []:
            rectangle = _route_rect(value)
            if rectangle:
                excluded.append(rectangle)
        if include_devices:
            for device in self.devices:
                bounds = device.get("bounds")
                if not isinstance(bounds, dict):
                    continue
                rectangle = (
                    int(bounds.get("x", 0)),
                    int(bounds.get("y", 0)),
                    int(bounds.get("width", 0)),
                    int(bounds.get("height", 0)),
                )
                if rectangle in excluded:
                    continue
                obstacles.append(rectangle)
            for instance in self.instances:
                box = _instance_world_bounds(instance)
                rectangle = (box[0], box[1], box[2] - box[0], box[3] - box[1])
                if rectangle in excluded or rectangle[2] <= 0 or rectangle[3] <= 0:
                    continue
                obstacles.append(rectangle)
        if include_shapes:
            for shape in self.shapes:
                if shape.get("layer") != layer_name:
                    continue
                shape_net = shape.get("net")
                if not shape_net or shape_net == net:
                    continue
                bounds = _route_record_bounds(shape)
                if bounds:
                    obstacles.append(bounds)
        if extra is not None:
            if isinstance(extra, dict):
                items = [extra]
            elif isinstance(extra, (list, tuple)):
                if len(extra) == 4 and all(
                    isinstance(value, (int, float)) for value in extra
                ):
                    items = [extra]
                else:
                    items = list(extra)
            else:
                items = [extra]
            for item in items:
                rectangle = _route_rect(item)
                if rectangle:
                    obstacles.append(rectangle)
        return obstacles

    def route(
        self,
        start,
        end,
        layer=None,
        width=None,
        net=None,
        start_side=None,
        end_side=None,
        obstacles=None,
        clearance=None,
        escape=None,
        bend_penalty=None,
        include_devices=True,
        include_shapes=True,
        exclude_device_bounds=None,
        layers=None,
        via_layers=None,
        via_size=None,
        via_enclosure=None,
        via_cost=None,
        start_layer=None,
        end_layer=None,
    ):
        """Route one Manhattan connection between two points or rectangles.

        Use ``layer`` for a single-layer route, or ``layers=[...]`` to route
        through a vertical layer stack. When both are omitted, the PDK
        package's declared routing stack is used. In a multi-layer route the
        router may insert via stacks between adjacent layers. ``via_layers``
        maps a ``(lower, upper)`` pair to one cut layer or a list of cut layers;
        when omitted, the package connectivity document is consulted first and
        common names such as ``via``/``via2``, ``Via1``/``Via2`` and
        ``Metal1``/``Metal2`` are used as a fallback.

        Endpoints may be ``(x, y)`` points, ``(x, y, width, height)`` port
        rectangles, or port tuples from a :class:`DevicePlacement`. Rectangle
        endpoints escape from the requested or nearest side by ``escape``.
        The router is deterministic and raises an error instead of emitting a
        short when no legal path exists.

        :param start: Start point or port rectangle.
        :param end: End point or port rectangle.
        :param layer: Single routing layer name or :class:`Layer` handle.
        :param width: Route width in database units (default 0.4 µm).
        :param net: Optional net name carried by the emitted shapes.
        :param start_side: Preferred escape side for a start rectangle:
            ``left``, ``right``, ``top``, or ``bottom``.
        :param end_side: Preferred escape side for an end rectangle.
        :param obstacles: Extra obstacle rectangle(s), a list, or a mapping
            from layer name to obstacle rectangles.
        :param clearance: Edge-to-edge clearance in database units (default
            ``max(width, 0.2 µm)``).
        :param escape: Port escape distance (default ``max(2 * width, 0.5 µm)``).
        :param bend_penalty: Extra cost per bend (default
            ``max(2 * width, 1 µm)``).
        :param include_devices: Treat placed device bounds as obstacles.
        :param include_shapes: Treat other-net layout shapes as obstacles.
        :param exclude_device_bounds: Device bounds that should not block the
            route even when ``include_devices`` is true.
        :param layers: Vertical layer stack, bottom to top.
        :param via_layers: Mapping of adjacent layer pairs to via cut layer(s).
        :param via_size: Via cut size in database units (default 0.15 µm).
        :param via_enclosure: Via enclosure in database units (default 0.1 µm).
        :param via_cost: Cost of changing layers (default
            ``max(4 * width, 1 µm)``).
        :param start_layer: Explicit start layer when using a stack.
        :param end_layer: Explicit end layer when using a stack.
        :returns: A :class:`Route` describing the emitted path.
        :raises RuntimeError: If no legal path or via transition exists.
        :raises ValueError: If endpoints, layers, or side arguments are
            invalid.
        """
        self._check_compaction_gate()
        if layer is None and layers is None:
            layers = self.routing_stack()
        layer_names = self._normalize_route_layers(layer, layers)
        if width is None:
            width = self.um(0.4)
        width = _positive(width, "route width")
        if clearance is None:
            clearance = max(width, self.um(0.2))
        else:
            # An explicit clearance is edge-to-edge spacing and may be smaller
            # than the trace width; only clamp it to something non-negative.
            clearance = max(0, _coordinate(clearance, "route clearance"))
        if escape is None:
            escape = max(width * 2, self.um(0.5))
        else:
            escape = _coordinate(escape, "route escape")
        escape = max(escape, clearance + width)
        if bend_penalty is None:
            bend_penalty = max(width * 2, 1)
        else:
            bend_penalty = _coordinate(bend_penalty, "route bend penalty")
        margin = max(width * 4, self.um(1))
        via_size_explicit = via_size is not None
        via_enclosure_explicit = via_enclosure is not None
        via_size = (
            self.um(0.15) if via_size is None else _positive(via_size, "via size")
        )
        via_enclosure = (
            self.um(0.1)
            if via_enclosure is None
            else _coordinate(via_enclosure, "via enclosure")
        )
        via_pad = max(width, via_size + 2 * via_enclosure)
        via_cost = (
            max(width * 4, self.um(1))
            if via_cost is None
            else _coordinate(via_cost, "via cost")
        )
        via_cost = max(via_cost, 1)

        start_rectangle = _route_rect(start)
        end_rectangle = _route_rect(end)
        start_point = _route_point(start)
        end_point = _route_point(end)
        if start_rectangle is None and start_point is None:
            raise ValueError("route start must be (x, y) or (x, y, width, height)")
        if end_rectangle is None and end_point is None:
            raise ValueError("route end must be (x, y) or (x, y, width, height)")

        start_device, start_info = (
            self._device_port_for_rectangle(start_rectangle)
            if start_rectangle
            else (None, None)
        )
        end_device, end_info = (
            self._device_port_for_rectangle(end_rectangle)
            if end_rectangle
            else (None, None)
        )

        def resolve_layer(explicit, info, default):
            if explicit is not None:
                name = self._layer_name(explicit)
                if name not in layer_names:
                    raise ValueError(f"route layer '{name}' is not in the layer stack")
                if info and info.get("layer") and info["layer"] != name:
                    raise ValueError(
                        f"port layer '{info['layer']}' does not match route layer '{name}'"
                    )
                return name
            if info and info.get("layer"):
                if info["layer"] in layer_names:
                    return info["layer"]
                raise ValueError(
                    f"port layer '{info['layer']}' is not in the route layer stack"
                )
            return default

        start_layer_name = resolve_layer(start_layer, start_info, layer_names[0])
        end_layer_name = resolve_layer(end_layer, end_info, layer_names[0])
        start_layer_index = layer_names.index(start_layer_name)
        end_layer_index = layer_names.index(end_layer_name)

        start_center = (
            _route_center(start_rectangle) if start_rectangle else start_point
        )
        end_center = _route_center(end_rectangle) if end_rectangle else end_point

        excluded_bounds = []
        for value in exclude_device_bounds or []:
            rectangle = _route_rect(value)
            if rectangle and rectangle not in excluded_bounds:
                excluded_bounds.append(rectangle)
        for device in (start_device, end_device):
            if device:
                bounds = _route_rect(device.get("bounds"))
                if bounds and bounds not in excluded_bounds:
                    excluded_bounds.append(bounds)

        obstacles_by_layer = {}
        for layer_name in layer_names:
            extra = self._route_extra_for_layer(obstacles, layer_name)
            obstacles_by_layer[layer_name] = self._route_obstacles(
                layer_name,
                net,
                include_devices,
                include_shapes,
                extra,
                exclude_device_bounds=excluded_bounds,
            )
        inflated_by_layer = [
            _route_inflate(obstacles_by_layer[name], clearance + width // 2)
            for name in layer_names
        ]

        via_map = self._normalize_via_map(via_layers)
        via_options = {}
        for index in range(len(layer_names) - 1):
            lower = layer_names[index]
            upper = layer_names[index + 1]
            cuts = via_map.get((lower, upper))
            if cuts is None:
                inferred = self._infer_via_layer(lower, upper)
                if inferred:
                    cuts = [inferred]
            if not cuts:
                continue
            cut_layers = [self._layer_name(cut) for cut in cuts]
            declared_via = self._declared_via(lower, upper)
            pair_size = via_size
            pair_enclosure = via_enclosure
            if declared_via:
                if not via_size_explicit and declared_via.get("size"):
                    pair_size = _positive(self.nm(declared_via["size"]), "via size")
                if (
                    not via_enclosure_explicit
                    and declared_via.get("enclosure") is not None
                ):
                    pair_enclosure = _coordinate(
                        self.nm(declared_via["enclosure"]), "via enclosure"
                    )
            pair_pad = max(width, pair_size + 2 * pair_enclosure)
            metal_inflate = clearance + pair_pad // 2
            cut_inflate = clearance + pair_size // 2
            lower_obstacles = _route_inflate(obstacles_by_layer[lower], metal_inflate)
            upper_obstacles = _route_inflate(obstacles_by_layer[upper], metal_inflate)
            cut_obstacles = []
            for cut in cut_layers:
                cut_extra = self._route_extra_for_layer(obstacles, cut)
                cut_obstacles.append(
                    _route_inflate(
                        self._route_obstacles(
                            cut,
                            net,
                            False,
                            include_shapes,
                            cut_extra,
                            exclude_device_bounds=excluded_bounds,
                        ),
                        cut_inflate,
                    )
                )

            def legal(x, y, lo=lower_obstacles, uo=upper_obstacles, co=cut_obstacles):
                if _route_inside_obstacle(x, y, lo) or _route_inside_obstacle(x, y, uo):
                    return False
                return not any(
                    _route_inside_obstacle(x, y, obstacle) for obstacle in co
                )

            option = {
                "lower": lower,
                "upper": upper,
                "via_layers": cut_layers,
                "legal": legal,
                "cost": via_cost,
                "pad": pair_pad,
                "cut": pair_size,
            }
            via_options[(index, index + 1)] = option
            via_options[(index + 1, index)] = option

        def layer_reachable(start_index, end_index):
            if start_index == end_index:
                return True
            seen = {start_index}
            stack = [start_index]
            while stack:
                current = stack.pop()
                for lower_index, upper_index in via_options:
                    if lower_index == current and upper_index not in seen:
                        seen.add(upper_index)
                        stack.append(upper_index)
                    elif upper_index == current and lower_index not in seen:
                        seen.add(lower_index)
                        stack.append(lower_index)
            return end_index in seen

        if not layer_reachable(start_layer_index, end_layer_index):
            raise RuntimeError(
                "no via layer connects the requested routing layers; pass via_layers"
            )

        if start_rectangle:
            preferred = start_side or _route_side_toward(start_center, end_center)
            if preferred not in {"left", "right", "top", "bottom"}:
                raise ValueError("start side must be left, right, top, or bottom")
            _, start_center, start_escape = _route_choose_side(
                start_rectangle,
                preferred,
                end_center,
                escape,
                inflated_by_layer[start_layer_index],
            )
        else:
            start_escape = start_center
        if end_rectangle:
            preferred = end_side or _route_side_toward(end_center, start_center)
            if preferred not in {"left", "right", "top", "bottom"}:
                raise ValueError("end side must be left, right, top, or bottom")
            _, end_center, end_escape = _route_choose_side(
                end_rectangle,
                preferred,
                start_center,
                escape,
                inflated_by_layer[end_layer_index],
            )
        else:
            end_escape = end_center

        path = _route_manhattan_layers(
            start_escape,
            start_layer_index,
            end_escape,
            end_layer_index,
            inflated_by_layer,
            via_options,
            via_cost,
            bend_penalty,
            margin,
        )
        full_points = []
        if start_rectangle:
            full_points.append((start_center[0], start_center[1], start_layer_index))
        full_points.extend(path)
        if end_rectangle:
            full_points.append((end_center[0], end_center[1], end_layer_index))
        full_points = _route_simplify_layers(full_points)

        route_index = self._route_count
        self._route_count += 1
        point_layers = [layer_names[point[2]] for point in full_points]
        points = [(point[0], point[1]) for point in full_points]

        segments = []
        vias = []
        for index in range(len(full_points) - 1):
            first = full_points[index]
            second = full_points[index + 1]
            if first[2] != second[2]:
                if (first[0], first[1]) != (second[0], second[1]):
                    raise RuntimeError("layer change must occur at a single point")
                lower_index = min(first[2], second[2])
                upper_index = max(first[2], second[2])
                option = via_options.get((lower_index, upper_index))
                if option is None:
                    raise RuntimeError("no via layer for the requested layer change")
                vias.append(
                    {
                        "x": first[0],
                        "y": first[1],
                        "lower": layer_names[lower_index],
                        "upper": layer_names[upper_index],
                        "via_layers": option["via_layers"],
                        "pad_size": option["pad"],
                        "cut_size": option["cut"],
                    }
                )
                continue
            if (first[0], first[1]) == (second[0], second[1]):
                continue
            segments.append(
                {
                    "start": (first[0], first[1]),
                    "end": (second[0], second[1]),
                    "layer": layer_names[first[2]],
                }
            )

        shapes = []
        segment_index = 0
        run_start = 0
        while run_start < len(full_points):
            run_end = run_start
            while (
                run_end + 1 < len(full_points)
                and full_points[run_end + 1][2] == full_points[run_start][2]
            ):
                run_end += 1
            for index in range(run_start, run_end):
                first = full_points[index]
                second = full_points[index + 1]
                extend_start = width // 2 if index > run_start else 0
                extend_end = width // 2 if index < run_end - 1 else 0
                name = f"{net or 'route'}_{route_index}_{segment_index}"
                segment_index += 1
                if first[1] == second[1]:
                    low, high = sorted((first[0], second[0]))
                    low -= extend_start
                    high += extend_end
                    shapes.append(
                        self.rectangle(
                            layer_names[first[2]],
                            low,
                            first[1] - width // 2,
                            high - low,
                            width,
                            name=name,
                            net=net,
                        )
                    )
                else:
                    low, high = sorted((first[1], second[1]))
                    low -= extend_start
                    high += extend_end
                    shapes.append(
                        self.rectangle(
                            layer_names[first[2]],
                            first[0] - width // 2,
                            low,
                            width,
                            high - low,
                            name=name,
                            net=net,
                        )
                    )
            run_start = run_end + 1

        for via_index, via in enumerate(vias):
            pad = via["pad_size"]
            cut = via["cut_size"]
            for layer_name, suffix in (
                (via["lower"], "lower"),
                (via["upper"], "upper"),
            ):
                shapes.append(
                    self.rectangle(
                        layer_name,
                        via["x"] - pad // 2,
                        via["y"] - pad // 2,
                        pad,
                        pad,
                        name=f"{net or 'route'}_{route_index}_via{via_index}_{suffix}",
                        net=net,
                    )
                )
            for cut_layer in via["via_layers"]:
                shapes.append(
                    self.rectangle(
                        cut_layer,
                        via["x"] - cut // 2,
                        via["y"] - cut // 2,
                        cut,
                        cut,
                        name=f"{net or 'route'}_{route_index}_via{via_index}_cut",
                        net=net,
                    )
                )

        length = sum(
            abs(second["end"][0] - second["start"][0])
            + abs(second["end"][1] - second["start"][1])
            for second in segments
        )
        bends = 0
        for index in range(1, len(full_points) - 1):
            before = full_points[index - 1]
            current = full_points[index]
            after = full_points[index + 1]
            if before[2] != current[2] or current[2] != after[2]:
                continue
            if (before[0] == current[0]) != (current[0] == after[0]):
                bends += 1
        route = Route(
            net,
            layer_names[start_layer_index],
            width,
            points,
            point_layers,
            segments,
            vias,
            shapes,
            length,
            bends,
            layer_names,
        )
        route._owner = self
        self._route_handles.append(route)
        return route

    def connect(
        self,
        device_a,
        pin_a,
        device_b,
        pin_b,
        layer=None,
        width=None,
        net=None,
        obstacles=None,
        clearance=None,
        escape=None,
        bend_penalty=None,
        include_devices=True,
        include_shapes=True,
        layers=None,
        via_layers=None,
        via_size=None,
        via_enclosure=None,
        via_cost=None,
    ):
        """Route directly between two ports of placed devices or layout instances.

        ``device_a`` and ``device_b`` may be :class:`DevicePlacement` objects
        returned by :meth:`device` or :class:`LayoutInstancePlacement` handles
        returned by :meth:`instance`. For a single-layer connection,
        ``layer`` is optional when both ports already share a layer. For ports
        on different layers, pass ``layers=[...]`` in vertical stack order
        and, if the package does not expose inferable via names, a
        ``via_layers`` mapping. The net is inferred from the port metadata
        when ``net`` is omitted, and connecting ports on different nets is
        rejected.

        :param device_a: First placed device or layout instance.
        :param pin_a: Port name on ``device_a`` (for example ``"G"``).
        :param device_b: Second placed device or layout instance.
        :param pin_b: Port name on ``device_b``.
        :param layer: Single routing layer for a same-layer connection.
        :param width: Route width in database units (default 0.4 µm).
        :param net: Optional net name; must agree with port nets when set.
        :param obstacles: Extra obstacle rectangle(s), a list, or a mapping
            from layer name to obstacle rectangles.
        :param clearance: Edge-to-edge clearance in database units.
        :param escape: Port escape distance in database units.
        :param bend_penalty: Extra cost per bend.
        :param include_devices: Treat other device bounds as obstacles.
        :param include_shapes: Treat other-net layout shapes as obstacles.
        :param layers: Vertical layer stack, bottom to top.
        :param via_layers: Mapping of adjacent layer pairs to via cut layer(s).
        :param via_size: Via cut size in database units.
        :param via_enclosure: Via enclosure in database units.
        :param via_cost: Cost of changing layers.
        :returns: A :class:`Route` describing the emitted connection.
        :raises TypeError: If an argument is not a device or layout instance.
        :raises ValueError: If a port or layer is unknown, or the ports belong
            to different nets.
        :raises RuntimeError: If no legal path or via transition exists.
        """
        self._check_compaction_gate()
        placements = (DevicePlacement, LayoutInstancePlacement)
        if not isinstance(device_a, placements) or not isinstance(device_b, placements):
            raise TypeError(
                "connect expects DevicePlacement or LayoutInstancePlacement objects"
            )
        info_a = device_a.port_info.get(str(pin_a))
        info_b = device_b.port_info.get(str(pin_b))
        if info_a is None:
            raise ValueError(f"device '{device_a.name}' has no port '{pin_a}'")
        if info_b is None:
            raise ValueError(f"device '{device_b.name}' has no port '{pin_b}'")
        layer_a = info_a.get("layer")
        layer_b = info_b.get("layer")
        if layers is None:
            if layer is None:
                if layer_a and layer_b and layer_a != layer_b:
                    raise ValueError(
                        "connect ports are on different layers; pass layers=[...] "
                        "and optionally via_layers=..."
                    )
                layer = layer_a or layer_b
            if layer is None:
                raise ValueError(
                    "connect requires a layer for ports without layer metadata"
                )
            layer_name = self._layer_name(layer)
            for label, info in (("start", info_a), ("end", info_b)):
                pin_layer = info.get("layer")
                if pin_layer and pin_layer != layer_name:
                    raise ValueError(
                        f"{label} port is on {pin_layer}, not {layer_name}; "
                        "pass layers=[...] and a via layer mapping"
                    )
            layer_names = [layer_name]
            start_layer_name = layer_name
            end_layer_name = layer_name
        else:
            if layer is not None:
                raise ValueError("pass either layer or layers, not both")
            layer_names = self._normalize_route_layers(None, layers)
            if not layer_a or not layer_b:
                raise ValueError("connect ports need layer metadata for a layer stack")
            if layer_a not in layer_names or layer_b not in layer_names:
                raise ValueError("both port layers must be included in the layers stack")
            start_layer_name = layer_a
            end_layer_name = layer_b
        start_net = info_a.get("net")
        end_net = info_b.get("net")
        if net is None:
            if start_net and end_net and start_net != end_net:
                raise ValueError(f"cannot connect net '{start_net}' to net '{end_net}'")
            net = start_net or end_net
        else:
            if start_net and start_net != net:
                raise ValueError(f"start port belongs to net '{start_net}', not '{net}'")
            if end_net and end_net != net:
                raise ValueError(f"end port belongs to net '{end_net}', not '{net}'")
        if width is None:
            width = self.um(0.4)
        return self.route(
            info_a["rectangle"],
            info_b["rectangle"],
            layers=layer_names,
            width=width,
            net=net,
            start_side=info_a.get("side"),
            end_side=info_b.get("side"),
            obstacles=obstacles,
            clearance=clearance,
            escape=escape,
            bend_penalty=bend_penalty,
            include_devices=include_devices,
            include_shapes=include_shapes,
            exclude_device_bounds=[device_a.bounds, device_b.bounds],
            via_layers=via_layers,
            via_size=via_size,
            via_enclosure=via_enclosure,
            via_cost=via_cost,
            start_layer=start_layer_name,
            end_layer=end_layer_name,
        )

    # -- composition and inspection --------------------------------------
    @property
    def constraints(self):
        """Constraint metadata recorded by :meth:`constraint` and helpers."""
        return list(self._constraint_records)

    @property
    def generator_metadata(self):
        """Metadata for generators instantiated by this builder."""
        return [_json_safe(dict(entry)) for entry in self._used_generators.values()]

    @property
    def is_empty(self):
        """Whether this builder or group contains no records."""
        return (
            not self.shapes
            and not self.ports
            and not self.instances
            and not self.devices
        )

    @property
    def width(self):
        """Bounding-box width of the builder or group."""
        bounds = _fragment_bounds(self)
        return 0 if bounds is None else bounds[2] - bounds[0]

    @property
    def height(self):
        """Bounding-box height of the builder or group."""
        bounds = _fragment_bounds(self)
        return 0 if bounds is None else bounds[3] - bounds[1]

    def move(self, dx, dy):
        """Translate every record in this builder and return ``self``."""
        self._layout_state["manual_moves"] += 1
        dx = _coordinate(dx, "move dx")
        dy = _coordinate(dy, "move dy")
        for record in self.shapes:
            _translate_record(record, dx, dy)
        for port in self.ports:
            rectangle = port.get("rectangle")
            if isinstance(rectangle, dict):
                rectangle["x"] += dx
                rectangle["y"] += dy
        for instance in self.instances:
            instance["x"] += dx
            instance["y"] += dy
        for device in self.devices:
            bounds = device.get("bounds")
            if isinstance(bounds, dict):
                bounds["x"] += dx
                bounds["y"] += dy
            for port in device.get("ports", []):
                rectangle = port.get("rectangle")
                if isinstance(rectangle, dict):
                    rectangle["x"] += dx
                    rectangle["y"] += dy
        for handle in self._device_handles:
            handle.refresh()
        for route in self._route_handles:
            _translate_route_metadata(route, dx, dy)
        return self

    def mirror(self, axis="y"):
        """Mirror every record about its own bounds center."""
        bounds = _fragment_bounds(self)
        _mirror_block(_block_from_layout(self), axis)
        for handle in self._device_handles:
            handle.refresh()
        if bounds is not None:
            flip_x, flip_y = _axis_flags(axis)
            low_x, low_y, high_x, high_y = bounds
            for route in self._route_handles:
                _mirror_route_metadata(
                    route, flip_x, flip_y, low_x, high_x, low_y, high_y
                )
        return self

    def add(self, item, x=0, y=0, mirror=None, prefix=None):
        """Place a composed fragment into this builder.

        Shapes, devices, routes, groups, rows, columns, grids, stacks, and
        repeats are accepted. Placement copies the item's records; the source
        handle remains valid and can be placed again.
        """
        self._note_composition("add")
        detach = not isinstance(item, (Layout, _Container))
        block = _copy_handle_block(item, detach=detach)
        if mirror:
            _mirror_block(block, mirror)
        if prefix:
            _prefix_block(block, prefix)
        bounds = block.bounds
        if bounds is not None:
            _translate_block(
                block,
                _coordinate(x, "add x") - bounds[0],
                _coordinate(y, "add y") - bounds[1],
            )
        existing = {str(port.get("name", "")) for port in self.ports}
        names = [str(port.get("name", "")) for port in block.ports]
        conflicts = sorted(name for name in names if name and name in existing)
        duplicates = sorted(
            name for name in set(names) if name and names.count(name) > 1
        )
        if conflicts or duplicates:
            raise ValueError(
                "duplicate port name(s) while adding layout fragment: "
                + ", ".join(sorted(set(conflicts + duplicates)))
                + "; use add(..., prefix=...) to rename them"
            )
        _merge_block_into_layout(self, block)
        for handle in block.handles:
            if (
                isinstance(handle, DevicePlacement)
                and handle not in self._device_handles
            ):
                self._device_handles.append(handle)
            elif isinstance(handle, Route) and handle not in self._route_handles:
                self._route_handles.append(handle)
        return item

    def group(self, name=None):
        """Create a local-coordinate layout fragment."""
        self._note_composition("group")
        return Group(self, name=name)

    def row(self, gap=0, align="bottom", distribute="fixed", width=None, name=None):
        """Create a horizontal composition."""
        self._note_composition("row")
        return Row(
            self, gap=gap, align=align, distribute=distribute, width=width, name=name
        )

    def column(self, gap=0, align="left", distribute="fixed", height=None, name=None):
        """Create a vertical composition."""
        self._note_composition("column")
        return Column(
            self, gap=gap, align=align, distribute=distribute, height=height, name=name
        )

    def _grid(self, items=None, columns=1, x_gap=0, y_gap=0, align="center", name=None):
        """Create a fixed-column grid composition."""
        self._note_composition("grid")
        grid = Grid(
            self, columns=columns, x_gap=x_gap, y_gap=y_gap, align=align, name=name
        )
        for item in items or []:
            grid.add(item)
        return grid

    def stack(self, items=None, align="center", name=None):
        """Create an overlay composition."""
        self._note_composition("stack")
        stack = Stack(self, align=align, name=name)
        for item in items or []:
            stack.add(item)
        return stack

    def repeat(
        self,
        unit=None,
        count=1,
        axis="x",
        pitch="abut",
        mirror=False,
        alternate=False,
        name=None,
    ):
        """Repeat a layout fragment along one axis."""
        self._note_composition("repeat")
        return Repeat(
            self,
            unit=unit,
            count=count,
            axis=axis,
            pitch=pitch,
            mirror=mirror,
            alternate=alternate,
            name=name,
        )

    def spacer(self, distance=0, height=None):
        """Reserve a fixed gap inside a row or column."""
        self._note_composition("spacer")
        return _Spacer(distance, height=height)

    def stretch(self, weight=1):
        """Consume slack inside a row or column with an explicit size."""
        self._note_composition("stretch")
        return _Spacer(0, stretch=True, weight=weight)

    def extents(self, *items):
        """Return the union bounds of handles, or ``None`` when empty."""
        values = (
            items[0]
            if len(items) == 1 and isinstance(items[0], (list, tuple))
            else items
        )
        return _union_bounds([_handle_bounds(item) for item in values])

    def clearance(self, first, second):
        """Return the edge-to-edge gap between two handles.

        The result is negative when the bounding boxes overlap.
        """
        a = _handle_bounds(first)
        b = _handle_bounds(second)
        if a is None or b is None:
            raise ValueError("clearance requires non-empty handles")
        dx = max(a[0] - b[2], b[0] - a[2])
        dy = max(a[1] - b[3], b[1] - a[3])
        if dx >= 0 or dy >= 0:
            return max(dx, dy)
        return max(dx, dy)

    def overlaps(self, first, second):
        """Whether two handle bounds overlap with positive area."""
        a = _handle_bounds(first)
        b = _handle_bounds(second)
        if a is None or b is None:
            return False
        return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]

    def contains(self, outer, inner):
        """Whether ``outer`` fully contains ``inner``."""
        a = _handle_bounds(outer)
        b = _handle_bounds(inner)
        if a is None or b is None:
            return False
        return a[0] <= b[0] and a[1] <= b[1] and a[2] >= b[2] and a[3] >= b[3]

    def distance(self, first, second, axis="x"):
        """Return the center-to-center distance along ``axis``."""
        a = _handle_bounds(first)
        b = _handle_bounds(second)
        if a is None or b is None:
            raise ValueError("distance requires non-empty handles")
        if str(axis).lower() in {"x", "horizontal"}:
            return ((a[0] + a[2]) - (b[0] + b[2])) // 2
        return ((a[1] + a[3]) - (b[1] + b[3])) // 2

    def measure(self, *items):
        """Return a dictionary describing the bounds of the given items."""
        bounds = self.extents(*items)
        if bounds is None:
            return {"bounds": None, "width": 0, "height": 0, "area": 0}
        width = bounds[2] - bounds[0]
        height = bounds[3] - bounds[1]
        return {
            "bounds": bounds,
            "width": width,
            "height": height,
            "area": width * height,
        }

    def report(self, gap_threshold=None, print_report=False):
        """Summarize layout area, device utilization, rows, and row gaps.

        The report is intended as an agent-facing quality check. Devices are
        grouped into row bands by vertical center and similar height before
        gaps are measured, so a tall passive or routing blockage is not compared
        with every small device in the cell. A low device-area utilization or a
        large gap inside a row usually means fixed spacing was used where rows,
        columns, abutment, or constraints would pack the layout more tightly.

        :param gap_threshold: Horizontal gap in database units that counts as
            large. Defaults to 10 micrometers.
        :param print_report: Print the report as JSON so it appears in the
            procedural-run log.
        :returns: A JSON-compatible dictionary.
        """
        state = self._layout_state
        composition = dict(state["composition"])
        composition_used = any(
            composition.get(kind, 0)
            for kind in (
                "row",
                "column",
                "grid",
                "stack",
                "repeat",
                "add",
                "constraint",
                "generator",
            )
        )
        instance_boxes = [
            _instance_world_bounds(instance) for instance in self.instances
        ]
        bounds = _union_bounds(
            [_bounds_of_records(self.shapes, self.ports)] + instance_boxes
        )
        if bounds is None:
            bounds = _fragment_bounds(self)
        device_boxes = []
        for device in self.devices:
            device_bounds = device.get("bounds")
            if not isinstance(device_bounds, dict):
                continue
            device_boxes.append(
                (
                    str(device.get("name", "")),
                    int(device_bounds.get("x", 0)),
                    int(device_bounds.get("y", 0)),
                    int(device_bounds.get("width", 0)),
                    int(device_bounds.get("height", 0)),
                )
            )
        if bounds is None:
            report = {
                "bounds": None,
                "width": 0,
                "height": 0,
                "width_um": 0.0,
                "height_um": 0.0,
                "cell_area_um2": 0.0,
                "device_area_um2": 0.0,
                "instance_area_um2": 0.0,
                "occupied_area_um2": 0.0,
                "device_utilization": 0.0,
                "utilization": 0.0,
                "shape_count": 0,
                "port_count": 0,
                "device_count": 0,
                "instance_count": 0,
                "row_count": 0,
                "rows": [],
                "max_horizontal_gap_um": 0.0,
                "large_gaps": [],
                "placement_style": "empty",
                "composition_used": composition_used,
                "composition": composition,
                "manual_move_count": int(state["manual_moves"]),
                "plan_present": state["plan"] is not None,
                "plan_passed": bool(
                    state["plan"] and state["plan"].get("passed", False)
                ),
                "warnings": ["layout contains no geometry"],
            }
        else:
            width = bounds[2] - bounds[0]
            height = bounds[3] - bounds[1]
            area = width * height
            area_scale = (self.db_unit_nm * self.db_unit_nm) / 1_000_000.0
            device_area = sum(box[3] * box[4] for box in device_boxes)
            instance_area = sum(
                (box[2] - box[0]) * (box[3] - box[1]) for box in instance_boxes
            )
            occupied_area = device_area + instance_area
            threshold = (
                self.um(10)
                if gap_threshold is None
                else _coordinate(gap_threshold, "gap threshold")
            )

            # Build row bands from device centers and heights. Requiring a
            # similar height keeps a tall MIM capacitor or resistor out of the
            # row gap measurement for the small transistors beside it.
            rows = []
            for box in sorted(
                device_boxes, key=lambda item: (item[2] + item[4] // 2, item[1])
            ):
                _, _, y, _, box_height = box
                center = y + box_height // 2
                match = None
                for row in rows:
                    smaller = max(1, min(box_height, row["min_height"]))
                    tolerance = max(1, smaller // 2)
                    ratio = max(box_height, row["max_height"]) / smaller
                    if abs(center - row["center"]) <= tolerance and ratio <= 2.0:
                        match = row
                        break
                if match is None:
                    match = {
                        "center": center,
                        "min_height": box_height,
                        "max_height": box_height,
                        "devices": [],
                    }
                    rows.append(match)
                match["devices"].append(box)
                match["min_height"] = min(match["min_height"], box_height)
                match["max_height"] = max(match["max_height"], box_height)

            rows.sort(key=lambda row: row["center"], reverse=True)
            row_reports = []
            large_gaps = []
            maximum_gap = 0
            for row_index, row in enumerate(rows):
                devices = sorted(row["devices"], key=lambda item: item[1])
                left = min(box[1] for box in devices)
                right = max(box[1] + box[3] for box in devices)
                top = max(box[2] + box[4] for box in devices)
                bottom = min(box[2] for box in devices)
                occupied_width = sum(box[3] for box in devices)
                span = max(1, right - left)
                row_gaps = []
                for first, second in zip(devices, devices[1:]):
                    gap = second[1] - (first[1] + first[3])
                    if gap <= 0:
                        continue
                    maximum_gap = max(maximum_gap, gap)
                    row_gaps.append(
                        {
                            "left": first[0],
                            "right": second[0],
                            "gap_um": round(gap * self.db_unit_nm / 1000.0, 3),
                        }
                    )
                    if gap > threshold:
                        large_gaps.append(
                            {
                                "row": row_index,
                                "left": first[0],
                                "right": second[0],
                                "gap_um": round(gap * self.db_unit_nm / 1000.0, 3),
                            }
                        )
                row_reports.append(
                    {
                        "index": row_index,
                        "devices": [box[0] for box in devices],
                        "bounds_um": [
                            round(left * self.db_unit_nm / 1000.0, 3),
                            round(bottom * self.db_unit_nm / 1000.0, 3),
                            round(right * self.db_unit_nm / 1000.0, 3),
                            round(top * self.db_unit_nm / 1000.0, 3),
                        ],
                        "span_um": round(span * self.db_unit_nm / 1000.0, 3),
                        "occupied_width_um": round(
                            occupied_width * self.db_unit_nm / 1000.0, 3
                        ),
                        "row_utilization": round(occupied_width / span, 4),
                        "max_gap_um": round(
                            max((gap["gap_um"] for gap in row_gaps), default=0.0),
                            3,
                        ),
                        "gaps": row_gaps,
                    }
                )

            utilization = occupied_area / area if area else 0.0
            device_utilization = device_area / area if area else 0.0
            warnings = []
            if (device_boxes or instance_boxes) and utilization < 0.20:
                warnings.append(
                    f"occupied area is only {utilization * 100:.1f}% of cell area; "
                    "compact rows/columns and remove fixed routing channels"
                )
            if maximum_gap > threshold:
                warnings.append(
                    f"largest row gap is {maximum_gap * self.db_unit_nm / 1000.0:.1f} um; "
                    "start rows/columns at gap=0 and let the router space ports"
                )
            placement_style = (
                "composed"
                if composition_used
                else ("manual" if (device_boxes or instance_boxes) else "empty")
            )
            report = {
                "bounds": bounds,
                "width": width,
                "height": height,
                "width_um": round(width * self.db_unit_nm / 1000.0, 3),
                "height_um": round(height * self.db_unit_nm / 1000.0, 3),
                "cell_area_um2": round(area * area_scale, 3),
                "device_area_um2": round(device_area * area_scale, 3),
                "instance_area_um2": round(instance_area * area_scale, 3),
                "occupied_area_um2": round(occupied_area * area_scale, 3),
                "device_utilization": round(device_utilization, 4),
                "utilization": round(utilization, 4),
                "shape_count": len(self.shapes),
                "port_count": len(self.ports),
                "device_count": len(device_boxes),
                "instance_count": len(instance_boxes),
                "row_count": len(row_reports),
                "rows": row_reports,
                "max_horizontal_gap_um": round(
                    maximum_gap * self.db_unit_nm / 1000.0, 3
                ),
                "large_gaps": large_gaps,
                "placement_style": placement_style,
                "composition_used": composition_used,
                "composition": composition,
                "manual_move_count": int(state["manual_moves"]),
                "plan_present": state["plan"] is not None,
                "plan_passed": bool(
                    state["plan"] and state["plan"].get("passed", False)
                ),
                "warnings": warnings,
            }
        self._layout_state["last_quality"] = report
        if print_report:
            print(json.dumps(report, indent=2))
        return report

    def plan(
        self,
        devices=None,
        rows=None,
        columns=None,
        target_utilization=0.20,
        max_gap_um=10.0,
        require_composition=False,
        require_gate=True,
        notes=None,
    ):
        """Declare the placement plan that the compaction gate will verify.

        Call this before placing devices. The plan is intentionally simple:
        expected devices, row/column grouping, a minimum device-area
        utilization, a maximum row gap, and whether composition helpers are
        required. ``finish_placement()`` (or ``compact()``) must pass before
        ``route()`` or ``connect()`` is allowed when ``require_gate`` is true.
        """
        state = self._layout_state
        if isinstance(devices, int):
            device_spec = int(devices)
        elif devices is None:
            device_spec = None
        else:
            device_spec = [str(name) for name in devices]
        if isinstance(rows, int):
            row_spec = int(rows)
        elif rows is None:
            row_spec = None
        else:
            row_spec = [
                [str(name) for name in row]
                if isinstance(row, (list, tuple))
                else str(row)
                for row in rows
            ]
        plan = {
            "declared": True,
            "devices": device_spec,
            "rows": row_spec,
            "columns": _json_safe(columns),
            "target_utilization": (
                None if target_utilization is None else float(target_utilization)
            ),
            "max_gap_um": float(max_gap_um),
            "require_composition": bool(require_composition),
            "require_gate": bool(require_gate),
            "notes": "" if notes is None else str(notes),
            "passed": False,
            "issues": [],
        }
        state["plan"] = plan
        state["compaction_passed"] = False
        state["compaction_quality"] = None
        state["review"] = None
        return plan

    def finish_placement(self):
        """Run the compaction gate and open routing.

        The gate checks the declared plan against the current devices and
        quality report. A failed gate raises :class:`LayoutQualityError` and
        leaves routing blocked.
        """
        state = self._layout_state
        plan = state.get("plan")
        if not plan:
            raise LayoutQualityError(
                "layout.plan(...) is required before layout.finish_placement()"
            )
        quality = self.report()
        issues = []
        expected = plan.get("devices")
        actual_names = [str(device.get("name", "")) for device in self.devices]
        if isinstance(expected, int):
            if len(actual_names) != expected:
                issues.append(
                    f"plan expected {expected} devices but found {len(actual_names)}"
                )
        elif isinstance(expected, list) and expected:
            missing = sorted(set(expected) - set(actual_names))
            if missing:
                issues.append("planned device(s) missing: " + ", ".join(missing))
        target = plan.get("target_utilization")
        if target is not None and quality["utilization"] < float(target):
            issues.append(
                f"utilization {quality['utilization']:.1%} is below target {float(target):.1%}"
            )
        plan_gap = plan.get("max_gap_um")
        if plan_gap is not None and quality["max_horizontal_gap_um"] > float(plan_gap):
            issues.append(
                f"largest row gap {quality['max_horizontal_gap_um']:.1f} um exceeds "
                f"target {float(plan_gap):.1f} um"
            )
        if plan.get("require_composition") and not quality["composition_used"]:
            issues.append("composition helpers were required but not used")
        planned_rows = plan.get("rows")
        if isinstance(planned_rows, int):
            if quality["row_count"] != planned_rows:
                issues.append(
                    f"plan expected {planned_rows} row(s) but report found {quality['row_count']}"
                )
        elif isinstance(planned_rows, list) and planned_rows:
            if quality["row_count"] != len(planned_rows):
                issues.append(
                    f"plan expected {len(planned_rows)} row(s) but report found {quality['row_count']}"
                )
        plan["issues"] = issues
        if issues:
            plan["passed"] = False
            state["compaction_passed"] = False
            raise LayoutQualityError(
                "layout compaction gate failed: "
                + "; ".join(issues)
                + ". Compact the placement and call layout.finish_placement() again."
            )
        plan["passed"] = True
        quality["plan_passed"] = True
        state["compaction_passed"] = True
        state["compaction_quality"] = quality
        return quality

    compact = finish_placement

    def review(
        self,
        min_utilization=None,
        max_gap_um=None,
        require_composition=None,
        require_plan=False,
    ):
        """Run the separate layout-review step and record its result.

        Review is intentionally independent of the authoring flow: an agent can
        call it after routing, or a reviewer can call it on a generated layout.
        It returns a pass/fail record with actionable issues and the quality
        report used for the decision.
        """
        state = self._layout_state
        final_quality = self.report()
        quality = state.get("compaction_quality") or final_quality
        plan = state.get("plan")
        issues = list(quality.get("warnings", []))
        if require_plan and not plan:
            issues.append("no layout.plan(...) was declared")
        if plan:
            if not plan.get("passed"):
                issues.append("layout compaction gate has not passed")
            target = (
                plan.get("target_utilization")
                if min_utilization is None
                else min_utilization
            )
            if target is not None and quality["utilization"] < float(target):
                issues.append(
                    f"utilization {quality['utilization']:.1%} is below review target "
                    f"{float(target):.1%}"
                )
            gap = plan.get("max_gap_um") if max_gap_um is None else max_gap_um
            if gap is not None and quality["max_horizontal_gap_um"] > float(gap):
                issues.append(
                    f"largest row gap {quality['max_horizontal_gap_um']:.1f} um exceeds "
                    f"review target {float(gap):.1f} um"
                )
            if require_composition is None:
                require_composition = plan.get("require_composition", False)
        elif min_utilization is not None and quality["utilization"] < float(
            min_utilization
        ):
            issues.append(
                f"utilization {quality['utilization']:.1%} is below review target "
                f"{float(min_utilization):.1%}"
            )
        if max_gap_um is not None and quality["max_horizontal_gap_um"] > float(
            max_gap_um
        ):
            issues.append(
                f"largest row gap {quality['max_horizontal_gap_um']:.1f} um exceeds "
                f"review target {float(max_gap_um):.1f} um"
            )
        if require_composition and not quality["composition_used"]:
            issues.append("composition helpers were required but not used")
        issues = list(dict.fromkeys(issues))
        review = {
            "passed": not issues,
            "issues": issues,
            "quality": quality,
            "cell_quality": final_quality,
            "plan": plan,
        }
        state["review"] = review
        return review

    def _check_compaction_gate(self):
        state = self._layout_state
        plan = state.get("plan")
        if (
            plan
            and plan.get("require_gate", True)
            and not state.get("compaction_passed", False)
        ):
            raise LayoutQualityError(
                "layout compaction gate: call layout.finish_placement() (or "
                "layout.compact()) after placement and before routing"
            )

    def _finalize_layout_metadata(self):
        state = self._layout_state
        plan = state.get("plan")
        if (
            plan
            and plan.get("require_gate", True)
            and not state.get("compaction_passed", False)
        ):
            raise LayoutQualityError(
                "layout compaction gate: call layout.finish_placement() (or "
                "layout.compact()) after placement and before output"
            )
        quality = state.get("compaction_quality") or self.report()
        plan_output = None
        if plan:
            plan_output = {
                key: value for key, value in plan.items() if key not in {"quality"}
            }
        return quality, plan_output, state.get("review")

    def abut(self, first, second, edge="right", gap=0):
        """Move ``second`` against one edge of ``first``."""
        a = _handle_bounds(first)
        b = _handle_bounds(second)
        if a is None or b is None:
            raise ValueError("abut requires non-empty handles")
        edge = str(edge).lower()
        gap = _coordinate(gap, "abut gap")
        if edge in {"right", "east", "e"}:
            dx, dy = a[2] + gap - b[0], a[1] - b[1]
        elif edge in {"left", "west", "w"}:
            dx, dy = a[0] - gap - b[2], a[1] - b[1]
        elif edge in {"top", "north", "n"}:
            dx, dy = a[0] - b[0], a[3] + gap - b[1]
        elif edge in {"bottom", "south", "s"}:
            dx, dy = a[0] - b[0], a[1] - gap - b[3]
        else:
            raise ValueError("abut edge must be left, right, top, or bottom")
        _translate_handle(second, dx, dy)
        self._constraint_records.append({"type": "Abut", "edge": edge, "gap": gap})
        return second

    def distribute(self, items, axis="x", mode="equal"):
        """Distribute item centers evenly between the first and last item."""
        items = list(items)
        if len(items) < 2:
            return items
        boxes = [_handle_bounds(item) for item in items]
        if any(box is None for box in boxes):
            raise ValueError("distribute requires non-empty handles")
        horizontal = str(axis).lower() in {"x", "horizontal"}
        first_center = (
            (boxes[0][0] + boxes[0][2]) // 2
            if horizontal
            else (boxes[0][1] + boxes[0][3]) // 2
        )
        last_box = boxes[-1]
        last_center = (
            (last_box[0] + last_box[2]) // 2
            if horizontal
            else (last_box[1] + last_box[3]) // 2
        )
        step = (last_center - first_center) // (len(items) - 1)
        for index, item in enumerate(items[1:-1], start=1):
            target = first_center + step * index
            box = _handle_bounds(item)
            center = (box[0] + box[2]) // 2 if horizontal else (box[1] + box[3]) // 2
            delta = target - center
            _translate_handle(
                item, delta if horizontal else 0, 0 if horizontal else delta
            )
        self._constraint_records.append(
            {"type": "Distribute", "axis": str(axis), "mode": str(mode)}
        )
        return items

    def constraint(self, kind, **options):
        """Apply a deterministic, local layout constraint.

        Supported constraints are ``Align``, ``AlignInOrder``, ``Order``,
        ``Symmetric``, ``Group``, ``Boundary``, ``AspectRatio``, ``Spread``,
        ``Grid``, ``PortLocation``, and ``Generator``.
        """
        self._note_composition("constraint")
        name = str(kind).strip()
        items = options.pop("items", None)
        if items is None:
            items = []
        elif not isinstance(items, (list, tuple)):
            items = [items]
        items = list(items)
        record = {"type": name}
        for key, value in options.items():
            if isinstance(value, (str, int, float, bool)) or value is None:
                record[key] = value
            elif isinstance(value, list) and all(
                isinstance(entry, (str, int, float, bool)) for entry in value
            ):
                record[key] = value
        self._constraint_records.append(record)
        normalized = name.lower().replace("_", "")
        if normalized == "align":
            self._constraint_align(items, options.get("line", "h_any"))
        elif normalized == "aligninorder":
            self._constraint_order(
                items,
                options.get("direction", "left_to_right"),
                bool(options.get("abut", False)),
                options.get("gap", 0),
            )
            self._constraint_align(items, options.get("line", "h_any"))
        elif normalized == "order":
            self._constraint_order(
                items,
                options.get("direction", "left_to_right"),
                bool(options.get("abut", False)),
                options.get("gap", 0),
            )
        elif normalized == "symmetric":
            self._constraint_symmetric(
                items, options.get("pairs", []), options.get("axis", "vertical")
            )
        elif normalized == "group":
            return self._constraint_group(items, options.get("name"))
        elif normalized == "boundary":
            self._constraint_boundary(
                items,
                options.get("max_width"),
                options.get("max_height"),
                options.get("halo", 0),
            )
        elif normalized == "aspectratio":
            self._constraint_aspect_ratio(
                items,
                options.get("minimum", options.get("ratio_low")),
                options.get("maximum", options.get("ratio_high")),
            )
        elif normalized == "spread":
            self._constraint_spread(
                items,
                options.get("direction", "horizontal"),
                options.get("distance", 0),
            )
        elif normalized == "grid":
            self._constraint_grid(items or self, options.get("pitch", self.grid))
        elif normalized == "portlocation":
            self._constraint_port_location(
                options.get("port"),
                options.get("side", "left"),
                options.get("align", "center"),
            )
        elif normalized == "generator":
            pass
        else:
            raise ValueError(f"unknown layout constraint '{kind}'")
        return None

    def align(self, *items, line="h_any"):
        """Convenience wrapper for the ``Align`` constraint."""
        return self.constraint("Align", items=list(items), line=line)

    def order(self, items, direction="left_to_right", abut=False, gap=0):
        """Convenience wrapper for the ``Order`` constraint."""
        return self.constraint(
            "Order", items=list(items), direction=direction, abut=abut, gap=gap
        )

    def symmetric(self, pairs, axis="vertical"):
        """Convenience wrapper for the ``Symmetric`` constraint."""
        return self.constraint("Symmetric", pairs=pairs, axis=axis)

    def boundary(self, max_width=None, max_height=None, halo=0, items=None):
        """Validate that current geometry fits the requested boundary."""
        return self.constraint(
            "Boundary",
            items=list(items or []),
            max_width=max_width,
            max_height=max_height,
            halo=halo,
        )

    def _constraint_align(self, items, line):
        pairs = [(item, _handle_bounds(item)) for item in items]
        boxes = [box for _, box in pairs if box is not None]
        if len(boxes) < 2:
            return
        line = str(line).lower()
        valid_lines = {
            "h_top",
            "h_bottom",
            "h_center",
            "h_any",
            "top",
            "bottom",
            "center",
            "v_left",
            "v_right",
            "v_center",
            "v_any",
            "left",
            "right",
        }
        if line not in valid_lines:
            raise ValueError(
                "Align line must be h_top, h_bottom, h_center, v_left, "
                "v_right, v_center, or an any/edge alias"
            )
        horizontal = line in {
            "h_top",
            "h_bottom",
            "h_center",
            "h_any",
            "top",
            "bottom",
            "center",
        }
        if horizontal:
            if line in {"h_top", "top"}:
                target = max(box[3] for box in boxes)
            elif line in {"h_bottom", "bottom"}:
                target = min(box[1] for box in boxes)
            else:
                target = (
                    min(box[1] for box in boxes) + max(box[3] for box in boxes)
                ) // 2
        else:
            if line in {"v_left", "left"}:
                target = min(box[0] for box in boxes)
            elif line in {"v_right", "right"}:
                target = max(box[2] for box in boxes)
            else:
                target = (
                    min(box[0] for box in boxes) + max(box[2] for box in boxes)
                ) // 2
        for item, box in pairs:
            if box is None:
                continue
            if horizontal:
                if line in {"h_top", "top"}:
                    delta = target - box[3]
                elif line in {"h_bottom", "bottom"}:
                    delta = target - box[1]
                else:
                    delta = target - (box[1] + box[3]) // 2
                _translate_handle(item, 0, delta)
            else:
                if line in {"v_left", "left"}:
                    delta = target - box[0]
                elif line in {"v_right", "right"}:
                    delta = target - box[2]
                else:
                    delta = target - (box[0] + box[2]) // 2
                _translate_handle(item, delta, 0)

    def _constraint_order(self, items, direction, abut=False, gap=0):
        boxes = [_handle_bounds(item) for item in items]
        if any(box is None for box in boxes):
            return
        gap = 0 if abut else max(0, _coordinate(gap, "order gap"))
        direction = str(direction).lower()
        if direction in {"left_to_right", "horizontal", "ltr"}:
            cursor = min(box[0] for box in boxes)
            for item, box in zip(items, boxes):
                _translate_handle(item, cursor - box[0], 0)
                cursor += (box[2] - box[0]) + gap
        elif direction in {"right_to_left", "rtl"}:
            cursor = max(box[2] for box in boxes)
            for item, box in zip(items, boxes):
                _translate_handle(item, cursor - box[2], 0)
                cursor -= (box[2] - box[0]) + gap
        elif direction in {"bottom_to_top", "vertical", "btt"}:
            cursor = min(box[1] for box in boxes)
            for item, box in zip(items, boxes):
                _translate_handle(item, 0, cursor - box[1])
                cursor += (box[3] - box[1]) + gap
        elif direction in {"top_to_bottom", "ttb"}:
            cursor = max(box[3] for box in boxes)
            for item, box in zip(items, boxes):
                _translate_handle(item, 0, cursor - box[3])
                cursor -= (box[3] - box[1]) + gap
        else:
            raise ValueError(
                "order direction must be left_to_right, right_to_left, "
                "bottom_to_top, or top_to_bottom"
            )

    def _constraint_symmetric(self, items, pairs, axis):
        if pairs and len(pairs) == 2 and not isinstance(pairs[0], (list, tuple)):
            pairs = [pairs]
        if not pairs and len(items) >= 2:
            pairs = [
                [items[index], items[index + 1]]
                for index in range(0, len(items) - 1, 2)
            ]
        flip_x = str(axis).lower() in {"vertical", "v", "y"}
        for pair in pairs:
            if not isinstance(pair, (list, tuple)) or len(pair) != 2:
                raise ValueError("Symmetric pairs must contain two items")
            first = _handle_bounds(pair[0])
            second = _handle_bounds(pair[1])
            if first is None or second is None:
                continue
            first_center = ((first[0] + first[2]) // 2, (first[1] + first[3]) // 2)
            second_center = ((second[0] + second[2]) // 2, (second[1] + second[3]) // 2)
            if flip_x:
                axis_coordinate = (first_center[0] + second_center[0]) // 2
                _mirror_handle(pair[1], "y")
                moved = _handle_bounds(pair[1])
                target = 2 * axis_coordinate - first_center[0]
                _translate_handle(pair[1], target - (moved[0] + moved[2]) // 2, 0)
                moved = _handle_bounds(pair[1])
                _translate_handle(
                    pair[1], 0, first_center[1] - (moved[1] + moved[3]) // 2
                )
            else:
                axis_coordinate = (first_center[1] + second_center[1]) // 2
                _mirror_handle(pair[1], "x")
                moved = _handle_bounds(pair[1])
                target = 2 * axis_coordinate - first_center[1]
                _translate_handle(pair[1], 0, target - (moved[1] + moved[3]) // 2)
                moved = _handle_bounds(pair[1])
                _translate_handle(
                    pair[1], first_center[0] - (moved[0] + moved[2]) // 2, 0
                )

    def _constraint_group(self, items, name):
        group = Group(self, name=name)
        block = _RecordBlock()
        for item in items:
            block.append(_copy_handle_block(item, detach=False))
        _merge_block_into_layout(group, block)
        return group

    def _constraint_boundary(self, items, max_width, max_height, halo=0):
        bounds = self.extents(items) if items else _fragment_bounds(self)
        if bounds is None:
            return
        halo = _coordinate(halo, "boundary halo")
        width = bounds[2] - bounds[0] + 2 * halo
        height = bounds[3] - bounds[1] + 2 * halo
        if max_width is not None and width > _coordinate(max_width, "max_width"):
            raise ValueError(
                f"Boundary constraint failed: width {width} exceeds "
                f"max_width {int(max_width)}"
            )
        if max_height is not None and height > _coordinate(max_height, "max_height"):
            raise ValueError(
                f"Boundary constraint failed: height {height} exceeds "
                f"max_height {int(max_height)}"
            )

    def _constraint_aspect_ratio(self, items, minimum, maximum):
        bounds = self.extents(items) if items else _fragment_bounds(self)
        if bounds is None:
            return
        width = bounds[2] - bounds[0]
        height = bounds[3] - bounds[1]
        ratio = width / height if height else math.inf
        if minimum is not None and ratio < float(minimum):
            raise ValueError(
                f"AspectRatio constraint failed: {ratio:.3f} is below {minimum}"
            )
        if maximum is not None and ratio > float(maximum):
            raise ValueError(
                f"AspectRatio constraint failed: {ratio:.3f} is above {maximum}"
            )

    def _constraint_spread(self, items, direction, distance):
        boxes = [_handle_bounds(item) for item in items]
        if any(box is None for box in boxes):
            return
        distance = max(0, _coordinate(distance, "spread distance"))
        direction = str(direction).lower()
        if direction not in {"horizontal", "x", "vertical", "y"}:
            raise ValueError("Spread direction must be horizontal or vertical")
        horizontal = direction in {"horizontal", "x"}
        for index in range(1, len(items)):
            previous = boxes[index - 1]
            current = _handle_bounds(items[index])
            if horizontal:
                if current[1] < previous[3] and previous[1] < current[3]:
                    if current[0] < previous[2] + distance:
                        _translate_handle(
                            items[index], previous[2] + distance - current[0], 0
                        )
            else:
                if current[0] < previous[2] and previous[0] < current[2]:
                    if current[1] < previous[3] + distance:
                        _translate_handle(
                            items[index], 0, previous[3] + distance - current[1]
                        )
            boxes[index] = _handle_bounds(items[index])

    def _constraint_grid(self, items, pitch):
        pitch = max(1, _coordinate(pitch, "grid pitch"))
        for item in items:
            _snap_handle(item, pitch)

    def _constraint_port_location(self, name, side, align):
        if not name:
            raise ValueError("PortLocation requires a port name")
        for port in self.ports:
            if str(port.get("name")) != str(name):
                continue
            rectangle = port["rectangle"]
            bounds = _fragment_bounds(self)
            side = str(side).lower()
            if side not in {"left", "right", "top", "bottom"}:
                raise ValueError(
                    "PortLocation side must be left, right, top, or bottom"
                )
            if side in {"left", "right"}:
                rectangle["x"] = (
                    bounds[0] if side == "left" else bounds[2] - rectangle["width"]
                )
                if str(align).lower() == "center":
                    rectangle["y"] = (bounds[1] + bounds[3]) // 2 - rectangle[
                        "height"
                    ] // 2
            else:
                rectangle["y"] = (
                    bounds[1] if side == "bottom" else bounds[3] - rectangle["height"]
                )
                if str(align).lower() == "center":
                    rectangle["x"] = (bounds[0] + bounds[2]) // 2 - rectangle[
                        "width"
                    ] // 2
            port["side"] = side
            return
        raise ValueError(f"PortLocation references unknown port '{name}'")

    # -- generators ------------------------------------------------------
    def generator(self, **metadata):
        """Register a cell generator declared in the current script."""

        def decorator(function):
            name = str(metadata.get("name") or function.__name__)
            spec = dict(metadata)
            spec.setdefault("name", name)
            spec.setdefault("description", (function.__doc__ or "").strip())
            spec.setdefault("parameters", {})
            spec.setdefault("ports", [])
            spec["source"] = "script"
            self._generators[name] = function
            self._generator_metadata[name] = spec
            return function

        return decorator

    def generate(self, generator, **options):
        """Instantiate a registered or built-in cell generator."""
        self._note_composition("generator")
        generator = str(generator)
        function = self._generators.get(generator)
        metadata = self._generator_metadata.get(generator, {})
        if function is None:
            builtins = self._load_builtin_generators()
            entry = builtins.get(generator)
            if entry is None:
                raise ValueError(
                    f"unknown layout generator '{generator}'; register it with "
                    "@layout.generator or install a built-in/package generator"
                )
            function, metadata = entry
        params = copy.deepcopy(metadata.get("parameters", {}))
        params.update(options)
        context = Group(self, name=f"generator_{generator}")
        result = function(context, **params)
        if result is None:
            result = context
        self._used_generators[generator] = _json_safe(dict(metadata))
        return result

    def _load_builtin_generators(self):
        if self._builtin_generators is not None:
            return self._builtin_generators
        path = Path(__file__).resolve().parent / "generators" / "__init__.py"
        if not path.is_file():
            self._builtin_generators = {}
            return self._builtin_generators
        module_name = "ice_builtin_procedural_generators"
        module = sys.modules.get(module_name)
        if module is None:
            spec = importlib.util.spec_from_file_location(module_name, path)
            if spec is None or spec.loader is None:
                self._builtin_generators = {}
                return self._builtin_generators
            module = importlib.util.module_from_spec(spec)
            sys.modules[module_name] = module
            spec.loader.exec_module(module)
        generators = module.generators()
        if self.pdk_root:
            generators.update(self._load_package_generators(Path(self.pdk_root)))
        self._builtin_generators = generators
        return self._builtin_generators

    def _load_package_generators(self, pdk_root):
        """Load optional ``<pdk>/layout/generators/index.json`` entries."""
        index_path = pdk_root / "layout" / "generators" / "index.json"
        if not index_path.is_file():
            return {}
        try:
            entries = json.loads(index_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise RuntimeError(
                f"Unable to read package generator index {index_path}: {error}"
            ) from error
        if not isinstance(entries, list):
            raise RuntimeError(f"Package generator index {index_path} must be a list")
        loaded = {}
        for entry in entries:
            if not isinstance(entry, dict):
                raise RuntimeError(
                    f"Package generator index {index_path} entries must be objects"
                )
            name = str(entry.get("name", "")).strip()
            module_name = str(entry.get("module", "")).strip()
            symbol = str(entry.get("function") or entry.get("class") or name).strip()
            if not name or not module_name or not symbol:
                raise RuntimeError(
                    f"Package generator entry in {index_path} needs name, module, and function"
                )
            module_path = index_path.parent / module_name
            spec = importlib.util.spec_from_file_location(
                f"ice_package_generator_{name}", module_path
            )
            if spec is None or spec.loader is None:
                raise RuntimeError(
                    f"Unable to load package generator module {module_path}"
                )
            module = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = module
            module_directory = str(module_path.parent)
            if module_directory not in sys.path:
                sys.path.insert(0, module_directory)
            spec.loader.exec_module(module)
            function = getattr(module, symbol, None)
            if isinstance(function, type):
                try:
                    instance = function()
                except TypeError as error:
                    raise RuntimeError(
                        f"Package generator class {name} must be constructible "
                        f"without arguments: {error}"
                    ) from error
                function = getattr(instance, "generate", instance)
            if not callable(function):
                raise RuntimeError(
                    f"Package generator {name} has no callable '{symbol}' in {module_path}"
                )
            metadata = dict(entry)
            metadata.setdefault("name", name)
            metadata.setdefault("description", (function.__doc__ or "").strip())
            metadata.setdefault("parameters", {})
            metadata.setdefault("ports", [])
            metadata["source"] = "package"
            metadata.setdefault("version", 1)
            loaded[name] = (function, metadata)
        return loaded

    def add_dummy_fill(
        self, layers=None, density=None, deviation=None, area=None, **options
    ):
        """Queue a PDK-provided dummy metal fill pass.

        ICE runs the implementation shipped by the active PDK package after the
        script finishes, so the fill sees the complete layout. ``layers`` may be
        a layer name or a list of layer names. ``density`` and ``deviation`` are
        percentages and may be scalars or per-layer mappings. ``area`` optionally
        limits fill to ``(x0, y0, x1, y1)`` database units or a polygon point
        list. Additional keyword options are forwarded to the PDK implementation.
        """
        if isinstance(layers, str):
            layers = [layers]
        elif layers is not None:
            layers = [str(layer) for layer in layers]
        if layers is not None and not layers:
            raise ValueError("layers must not be empty")
        request = {
            "layers": layers,
            "density": density,
            "deviation": deviation,
            "area": area,
        }
        request.update(options)
        self.dummy_fill_requests.append(request)

    def iter_polygons(self, layer=None):
        """Yield ``(layer_name, [(x, y), ...])`` for every shape in the layout."""
        for shape in self.shapes:
            if layer is not None and shape["layer"] != layer:
                continue
            if "centerline" in shape:
                points = [(point["x"], point["y"]) for point in shape["centerline"]]
                width = shape["width"]
                for index in range(len(points) - 1):
                    polygon = _path_segment_polygon(
                        points[index], points[index + 1], width
                    )
                    if len(polygon) >= 3:
                        yield shape["layer"], polygon
                continue
            points = _shape_points(shape)
            if len(points) >= 3:
                yield shape["layer"], points

    def bounds(self, layer=None):
        """Return ``(min_x, min_y, max_x, max_y)`` for the selected geometry.

        With no layer filter the result includes ports and child instances, so
        a layout containing only an imported macro still reports its real
        extent. Passing a layer name limits the result to shapes on that layer.
        """
        if layer is None:
            record_bounds = _bounds_of_records(self.shapes, self.ports)
            instance_boxes = [
                _instance_world_bounds(instance) for instance in self.instances
            ]
            return _union_bounds([record_bounds] + instance_boxes)
        min_x = math.inf
        min_y = math.inf
        max_x = -math.inf
        max_y = -math.inf
        for _, points in self.iter_polygons(layer):
            for x, y in points:
                min_x = min(min_x, x)
                min_y = min(min_y, y)
                max_x = max(max_x, x)
                max_y = max(max_y, y)
        if not math.isfinite(min_x):
            return None
        return (int(min_x), int(min_y), int(max_x), int(max_y))

    def layer_number(self, name):
        """Return the GDS layer number registered for ``name``, if any."""
        if not isinstance(name, str):
            return None
        for layer_name, spec in self.layers.items():
            if layer_name == name:
                return spec["number"]
        lowered = name.lower()
        for layer_name, spec in self.layers.items():
            if layer_name.lower() == lowered:
                return spec["number"]
        return None

    # -- unit helpers -----------------------------------------------------
    def db(self, value):
        """Return ``value`` rounded to the nearest integer database unit."""
        return _coordinate(value, "database value")

    def nm(self, value):
        """Convert nanometers to integer database units.

        The conversion uses :attr:`db_unit_nm`, which is normally 1 for a
        1 nm database unit.
        """
        if self.db_unit_nm <= 0:
            raise ValueError("Database unit size must be positive")
        return _coordinate(
            float(_coordinate(value, "nanometers")) / self.db_unit_nm, "nanometers"
        )

    def um(self, value):
        """Convert micrometers to integer database units."""
        return self.nm(float(value) * 1000.0)

    def snap(self, value):
        """Snap a database-unit value to the project grid."""
        coordinate = _coordinate(value, "grid value")
        grid = max(1, self.grid)
        return int(round(coordinate / grid)) * grid

    def length(self, value):
        """Convert a length string or bare number to database units.

        Parses SPICE-style suffixes such as ``2.5u``, ``150n``, ``4k``, and
        ``1mm``. A bare number is interpreted as database units.
        """
        if isinstance(value, (int, float)):
            return _coordinate(value, "length")
        text = str(value).strip()
        suffixes = (
            ("meg", 1e6),
            ("mil", 25400.0),
            ("nm", 1.0),
            ("um", 1000.0),
            ("µm", 1000.0),
            ("mm", 1e6),
            ("m", 1e3),
            ("u", 1000.0),
            ("n", 1.0),
            ("p", 1e-3),
            ("f", 1e-6),
            ("k", 1e6),
        )
        for suffix, nanometers in suffixes:
            if text.endswith(suffix):
                try:
                    return self.nm(float(text[: -len(suffix)]) * nanometers)
                except ValueError:
                    break
        return self.db(float(text))

    def to_dict(self):
        """Return the builder output as a JSON-compatible dictionary.

        This is the document ICE validates and applies after the script
        finishes; scripts normally do not need to call it directly.
        """
        quality, plan, review = self._finalize_layout_metadata()
        return {
            "format": "ice-procedural-layout",
            "schema_version": SCHEMA_VERSION,
            "builder_version": BUILDER_VERSION,
            "db_unit_nm": self.db_unit_nm,
            "grid": self.grid,
            "generator_metadata": self.generator_metadata,
            "constraints": self.constraints,
            "layout_quality": _json_safe(quality),
            "layout_plan": _json_safe(plan),
            "layout_review": _json_safe(review),
            "layers": [
                {
                    "name": name,
                    "number": spec["number"],
                    "datatype": spec["datatype"],
                }
                for name, spec in self.layers.items()
            ],
            "parameters": list(self.parameter_specs.values()),
            "shapes": self.shapes,
            "ports": self.ports,
            "instances": self.instances,
            "devices": self.devices,
        }


class Group(Layout):
    """Local-coordinate layout fragment.

    A group has the same geometry, device, routing, and generator API as the
    root builder. Adding the group to a layout or composition copies its
    records, so one group can be instanced more than once.
    """

    def __init__(self, parent, name=None):
        super().__init__(
            parent.pdk,
            parent.pdk_root,
            parent.db_unit_nm,
            parent.grid,
            parent.params,
            parent.cells,
        )
        self.name = str(name) if name is not None else ""
        self._parent = parent
        self._generators = parent._generators
        self._generator_metadata = parent._generator_metadata
        self._used_generators = parent._used_generators
        self._constraint_records = parent._constraint_records
        self._builtin_generators = parent._builtin_generators
        self._layout_state = parent._layout_state

    @property
    def bounds(self):
        """Axis-aligned bounds of the group fragment."""
        return Layout.bounds(self)

    def __repr__(self):
        return f"Group({self.name or 'unnamed'}, {len(self.shapes)} shapes)"


class _Spacer:
    """Fixed or stretchable gap inside a row or column."""

    def __init__(self, distance=0, height=None, stretch=False, weight=1):
        self.distance = max(0, _coordinate(distance, "spacer distance"))
        self.height = (
            None if height is None else max(0, _coordinate(height, "spacer height"))
        )
        self.stretch = bool(stretch)
        self.weight = max(1, int(weight))

    @property
    def width(self):
        return 0 if self.stretch else self.distance

    @property
    def is_empty(self):
        return True

    @property
    def bounds(self):
        return (0, 0, self.width, self.height or 0)

    def move(self, dx, dy):
        return self

    def mirror(self, axis="y"):
        return self


class _Item:
    def __init__(self, block=None, kind="block", size=0, height=0, weight=1):
        self.block = block or _RecordBlock()
        self.kind = kind
        self.size = max(0, int(size))
        self._height = max(0, int(height))
        self.weight = max(1, int(weight))
        self.allocated = 0

    @property
    def bounds(self):
        if self.kind in {"spacer", "stretch"}:
            return (0, 0, self.size, self.height)
        return self.block.bounds

    @property
    def width(self):
        if self.kind in {"spacer", "stretch"}:
            return self.size
        return self.block.width

    @property
    def height(self):
        if self.kind in {"spacer", "stretch"}:
            return self._height
        return self.block.height


class _Container:
    """Base class for row, column, grid, stack, and repeat compositions."""

    def __init__(self, layout, name=None):
        self._layout = layout
        self.name = str(name) if name is not None else ""
        self._items = []
        self.shapes = []
        self.ports = []
        self.instances = []
        self.devices = []
        self.layers = {}
        self._extra_ports = []
        self._width = 0
        self._height = 0

    # Builder helpers used by units in scripts.
    def db(self, value):
        return self._layout.db(value)

    def nm(self, value):
        return self._layout.nm(value)

    def um(self, value):
        return self._layout.um(value)

    def snap(self, value):
        return self._layout.snap(value)

    def length(self, value):
        return self._layout.length(value)

    @property
    def bounds(self):
        return _union_bounds([item.bounds for item in self._items])

    @property
    def width(self):
        bounds = self.bounds
        return 0 if bounds is None else bounds[2] - bounds[0]

    @property
    def height(self):
        bounds = self.bounds
        return 0 if bounds is None else bounds[3] - bounds[1]

    @property
    def is_empty(self):
        return not self._items

    def port(
        self,
        name,
        layer,
        x,
        y,
        width,
        height,
        direction="inout",
        side="left",
        net=None,
        label=None,
        visible=True,
        locked=False,
    ):
        """Add a port to the composed fragment."""
        if not isinstance(name, str) or not name.strip():
            raise ValueError("Port name must be a non-empty string")
        if direction not in {"input", "output", "inout", "power", "ground"}:
            raise ValueError(
                "Port direction must be input, output, inout, power, or ground"
            )
        if side not in {"left", "right", "top", "bottom"}:
            raise ValueError("Port side must be left, right, top, or bottom")
        layer_name = layer.name if isinstance(layer, Layer) else str(layer).strip()
        if not layer_name:
            raise ValueError("Port layer must be a non-empty string")
        self.layers.setdefault(layer_name, {"number": None, "datatype": None})
        record = {
            "name": name.strip(),
            "layer": layer_name,
            "rectangle": {
                "x": _coordinate(x, "port x"),
                "y": _coordinate(y, "port y"),
                "width": _positive(width, "port width"),
                "height": _positive(height, "port height"),
            },
            "direction": direction,
            "side": side,
            "net": str(net).strip() if isinstance(net, str) and net.strip() else None,
            "label": str(label or name).strip(),
            "visible": bool(visible),
            "locked": bool(locked),
        }
        self._extra_ports.append(record)
        self.ports.append(record)
        return record

    def pin(self, name):
        for port in self.ports:
            if str(port.get("name")) == str(name):
                rectangle = port["rectangle"]
                return (
                    rectangle["x"],
                    rectangle["y"],
                    rectangle["width"],
                    rectangle["height"],
                )
        raise KeyError(f"composition has no port '{name}'")

    def anchor(self, name="center"):
        return _anchor_point(self.bounds, name)

    def add(self, item):
        """Add an item and reflow the composition."""
        if isinstance(item, _Spacer):
            kind = "stretch" if item.stretch else "spacer"
            self._items.append(
                _Item(
                    kind=kind,
                    size=item.distance,
                    height=item.height or 0,
                    weight=item.weight,
                )
            )
        else:
            detach = not isinstance(item, (Layout, _Container))
            self._items.append(_Item(block=_copy_handle_block(item, detach=detach)))
        self._reflow()
        return item

    def move(self, dx, dy):
        for item in self._items:
            if item.kind == "block":
                _translate_block(item.block, dx, dy)
        for port in self._extra_ports:
            rectangle = port.get("rectangle")
            if isinstance(rectangle, dict):
                rectangle["x"] += dx
                rectangle["y"] += dy
        self._refresh()
        return self

    def mirror(self, axis="y"):
        bounds = self.bounds
        for item in self._items:
            if item.kind == "block":
                _mirror_block(item.block, axis)
        if bounds is not None and self._extra_ports:
            flip_x, flip_y = _axis_flags(axis)
            _mirror_ports(
                self._extra_ports,
                flip_x,
                flip_y,
                bounds[0],
                bounds[2],
                bounds[1],
                bounds[3],
            )
        self._reflow()
        return self

    def _refresh(self):
        self.shapes = []
        self.ports = []
        self.instances = []
        self.devices = []
        self.layers = {}
        for item in self._items:
            if item.kind != "block":
                continue
            self.shapes.extend(item.block.shapes)
            self.ports.extend(item.block.ports)
            self.instances.extend(item.block.instances)
            self.devices.extend(item.block.devices)
            for name, spec in item.block.layers.items():
                self.layers.setdefault(name, dict(spec))
        self.ports.extend(self._extra_ports)
        for port in self._extra_ports:
            self.layers.setdefault(
                port.get("layer", ""), {"number": None, "datatype": None}
            )
        bounds = self.bounds
        self._width = 0 if bounds is None else bounds[2] - bounds[0]
        self._height = 0 if bounds is None else bounds[3] - bounds[1]

    def _reflow(self):
        self._refresh()

    def __iter__(self):
        return iter(self._items)

    def __len__(self):
        return len(self._items)


class Row(_Container):
    """Horizontal composition with optional abutment and slack distribution."""

    def __init__(
        self, layout, gap=0, align="bottom", distribute="fixed", width=None, name=None
    ):
        super().__init__(layout, name=name)
        self.gap = max(0, _coordinate(gap, "row gap"))
        self.align = str(align).lower()
        self.distribute = str(distribute).lower()
        self.target_width = (
            None if width is None else max(0, _coordinate(width, "row width"))
        )

    def _reflow(self):
        blocks = self._items
        if not blocks:
            self._refresh()
            return
        widths = [item.width for item in blocks]
        total = sum(widths) + self.gap * max(0, len(blocks) - 1)
        slack = max(0, (self.target_width or 0) - total)
        flex = [item for item in blocks if item.kind == "stretch"]
        gap_extra = 0
        if flex:
            share = slack // max(1, sum(item.weight for item in flex))
            for item in flex:
                item.allocated = share * item.weight
                item.size = item.allocated
        elif (
            slack and self.distribute in {"space-between", "equal"} and len(blocks) > 1
        ):
            gap_extra = slack // (len(blocks) - 1)
        cursor = 0
        for item in blocks:
            if item.kind in {"spacer", "stretch"}:
                cursor += item.width
                if item.kind != "stretch":
                    cursor += self.gap + gap_extra
                else:
                    cursor += self.gap
                continue
            box = item.block.bounds
            if box is None:
                cursor += self.gap
                continue
            _translate_block(item.block, cursor - box[0], 0)
            cursor += (box[2] - box[0]) + self.gap + gap_extra
        boxes = []
        for item in blocks:
            if item.kind == "block":
                box = item.block.bounds
                boxes.append(box)
        max_height = max((box[3] - box[1] for box in boxes if box), default=0)
        for item in blocks:
            if item.kind != "block":
                continue
            box = item.block.bounds
            if box is None:
                continue
            if self.align in {"top", "north", "n"}:
                delta = max_height - box[3]
            elif self.align in {"center", "middle", "c"}:
                delta = (max_height - (box[3] - box[1])) // 2 - box[1]
            else:
                delta = -box[1]
            _translate_block(item.block, 0, delta)
        self._refresh()


class Column(_Container):
    """Vertical composition with optional abutment and slack distribution."""

    def __init__(
        self, layout, gap=0, align="left", distribute="fixed", height=None, name=None
    ):
        super().__init__(layout, name=name)
        self.gap = max(0, _coordinate(gap, "column gap"))
        self.align = str(align).lower()
        self.distribute = str(distribute).lower()
        self.target_height = (
            None if height is None else max(0, _coordinate(height, "column height"))
        )

    def add(self, item):
        if isinstance(item, _Spacer):
            item.height = item.distance
        return super().add(item)

    def _reflow(self):
        blocks = self._items
        if not blocks:
            self._refresh()
            return
        heights = [
            item.size if item.kind in {"spacer", "stretch"} else item.height
            for item in blocks
        ]
        total = sum(heights) + self.gap * max(0, len(blocks) - 1)
        slack = max(0, (self.target_height or 0) - total)
        flex = [item for item in blocks if item.kind == "stretch"]
        gap_extra = 0
        if flex:
            share = slack // max(1, sum(item.weight for item in flex))
            for item in flex:
                item.allocated = share * item.weight
                item.size = item.allocated
        elif (
            slack and self.distribute in {"space-between", "equal"} and len(blocks) > 1
        ):
            gap_extra = slack // (len(blocks) - 1)
        cursor = 0
        for item in blocks:
            if item.kind in {"spacer", "stretch"}:
                cursor += item.size
                cursor += self.gap + gap_extra
                continue
            box = item.block.bounds
            if box is None:
                cursor += self.gap
                continue
            _translate_block(item.block, 0, cursor - box[1])
            cursor += (box[3] - box[1]) + self.gap + gap_extra
        boxes = [item.block.bounds for item in blocks if item.kind == "block"]
        max_width = max((box[2] - box[0] for box in boxes if box), default=0)
        for item in blocks:
            if item.kind != "block":
                continue
            box = item.block.bounds
            if box is None:
                continue
            if self.align in {"right", "east", "e"}:
                delta = max_width - box[2]
            elif self.align in {"center", "middle", "c"}:
                delta = (max_width - (box[2] - box[0])) // 2 - box[0]
            else:
                delta = -box[0]
            _translate_block(item.block, delta, 0)
        self._refresh()


class Grid(_Container):
    """Fixed-column grid of layout items."""

    def __init__(self, layout, columns=1, x_gap=0, y_gap=0, align="center", name=None):
        super().__init__(layout, name=name)
        self.columns = max(1, int(columns))
        self.x_gap = max(0, _coordinate(x_gap, "grid x gap"))
        self.y_gap = max(0, _coordinate(y_gap, "grid y gap"))
        self.align = str(align).lower()

    def _reflow(self):
        blocks = [item for item in self._items if item.kind == "block"]
        if not blocks:
            self._refresh()
            return
        rows = [
            blocks[index : index + self.columns]
            for index in range(0, len(blocks), self.columns)
        ]
        # Column widths are the maxima across rows.
        columns = self.columns
        column_widths = [0] * columns
        for row in rows:
            for index, item in enumerate(row):
                column_widths[index] = max(column_widths[index], item.width)
        row_heights = [max((item.height for item in row), default=0) for row in rows]
        y_cursor = 0
        for row_index, row in enumerate(rows):
            x_cursor = 0
            for column_index, item in enumerate(row):
                box = item.block.bounds
                if box is not None:
                    width = box[2] - box[0]
                    height = box[3] - box[1]
                    if self.align in {"center", "middle", "c"}:
                        x = (column_widths[column_index] - width) // 2
                        y = (row_heights[row_index] - height) // 2
                    elif self.align in {"top", "north", "n"}:
                        x, y = 0, row_heights[row_index] - height
                    elif self.align in {"right", "east", "e"}:
                        x, y = column_widths[column_index] - width, 0
                    else:
                        x, y = 0, 0
                    _translate_block(
                        item.block, x_cursor + x - box[0], y_cursor + y - box[1]
                    )
                x_cursor += column_widths[column_index] + self.x_gap
            y_cursor += row_heights[row_index] + self.y_gap
        self._refresh()


class Stack(_Container):
    """Overlay items on a common origin without merging geometry."""

    def __init__(self, layout, align="center", name=None):
        super().__init__(layout, name=name)
        self.align = str(align).lower()

    def _reflow(self):
        blocks = [item for item in self._items if item.kind == "block"]
        if not blocks:
            self._refresh()
            return
        for item in blocks:
            box = item.block.bounds
            if box is not None:
                _translate_block(item.block, -box[0], -box[1])
        extent = _union_bounds([item.block.bounds for item in blocks])
        if extent is None:
            self._refresh()
            return
        width = extent[2] - extent[0]
        height = extent[3] - extent[1]
        for item in blocks:
            box = item.block.bounds
            if box is None:
                continue
            item_width = box[2] - box[0]
            item_height = box[3] - box[1]
            if self.align in {"center", "middle", "c"}:
                dx = (width - item_width) // 2
                dy = (height - item_height) // 2
            elif self.align in {"top", "north", "n"}:
                dx, dy = 0, height - item_height
            elif self.align in {"bottom", "south", "s"}:
                dx, dy = 0, 0
            elif self.align in {"left", "west", "w"}:
                dx, dy = 0, 0
            elif self.align in {"right", "east", "e"}:
                dx, dy = width - item_width, 0
            else:
                dx, dy = 0, 0
            _translate_block(item.block, dx, dy)
        self._refresh()


class Repeat(_Container):
    """Repeated, optionally mirrored copies of a layout fragment."""

    def __init__(
        self,
        layout,
        unit=None,
        count=1,
        axis="x",
        pitch="abut",
        mirror=False,
        alternate=False,
        name=None,
    ):
        super().__init__(layout, name=name)
        self.axis = str(axis).lower()
        self.pitch = pitch
        self.mirror_copies = bool(mirror)
        self.alternate = bool(alternate)
        self.count = max(0, int(count))
        self._unit = unit
        if unit is not None:
            self._build()

    def add(self, item):
        if self._unit is not None:
            raise ValueError("repeat already has a unit; create another repeat")
        self._unit = item
        self._build()
        return item

    def _build(self):
        self._items = []
        if self._unit is None:
            self._refresh()
            return
        if isinstance(
            self._unit, (Shape, DevicePlacement, Route, LayoutInstancePlacement)
        ):
            _detach_handle_records(self._unit)
        horizontal = self.axis in {"x", "horizontal", "h"}
        pitch_text = str(self.pitch).lower()
        numeric_pitch = None
        if pitch_text != "abut":
            numeric_pitch = _coordinate(self.pitch, "repeat pitch")
        cursor = 0
        for index in range(self.count):
            block = _copy_handle_block(self._unit)
            if index:
                _suffix_block(block, f"_{index + 1}")
            if self.mirror_copies and (not self.alternate or index % 2 == 1):
                _mirror_block(block, "y" if horizontal else "x")
            box = block.bounds
            if box is not None:
                if horizontal:
                    target = cursor if numeric_pitch is None else index * numeric_pitch
                    _translate_block(block, target - box[0], -box[1])
                    cursor = (
                        target
                        + (box[2] - box[0])
                        + (0 if numeric_pitch is not None else 0)
                    )
                else:
                    target = cursor if numeric_pitch is None else index * numeric_pitch
                    _translate_block(block, -box[0], target - box[1])
                    cursor = target + (box[3] - box[1])
            self._items.append(_Item(block=block))
        self._refresh()


def _load_dummy_fill_extension(layout):
    if not layout.pdk_root:
        pdk = layout.pdk or "unknown"
        raise RuntimeError(
            f"PDK '{pdk}' does not provide a dummy fill implementation. "
            "The implementation is expected at <pdk-root>/layout/dummy_fill.py."
        )
    extension_path = Path(layout.pdk_root) / "layout" / "dummy_fill.py"
    if not extension_path.is_file():
        pdk = layout.pdk or "unknown"
        raise RuntimeError(
            f"PDK '{pdk}' does not provide {extension_path}. "
            "Install a PDK package with a layout/dummy_fill.py implementation."
        )
    spec = importlib.util.spec_from_file_location("ice_pdk_dummy_fill", extension_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(
            f"Unable to load the dummy fill implementation at {extension_path}"
        )
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    sys.path.insert(0, str(extension_path.parent))
    spec.loader.exec_module(module)
    return module


def _apply_dummy_fill_requests(layout):
    if not layout.dummy_fill_requests:
        return
    module = _load_dummy_fill_extension(layout)
    handler = getattr(module, "add_dummy_fill", None)
    if not callable(handler):
        raise RuntimeError(
            "The PDK dummy fill implementation has no add_dummy_fill() function."
        )
    for request in layout.dummy_fill_requests:
        handler(layout, **request)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--script", help="Procedural layout Python script")
    parser.add_argument("--output", help="Generated geometry JSON path")
    parser.add_argument(
        "--list-generators",
        action="store_true",
        help="Print built-in generator metadata as JSON and exit",
    )
    parser.add_argument(
        "--params-file", help="Optional procedural parameter values (JSON object)"
    )
    parser.add_argument(
        "--cells-file",
        help="Optional project layout-cell catalog (JSON array) for instances",
    )
    parser.add_argument("--pdk", default=None, help="Active PDK identifier")
    parser.add_argument("--pdk-root", default=None, help="Installed PDK package root")
    parser.add_argument(
        "--db-unit-nm",
        type=float,
        default=1.0,
        help="Database unit size in nanometers",
    )
    parser.add_argument(
        "--grid",
        type=float,
        default=1.0,
        help="Project grid in database units",
    )
    args = parser.parse_args()

    if args.list_generators:
        builder = Layout(args.pdk, args.pdk_root)
        entries = builder._load_builtin_generators()
        metadata = [
            dict(entry[1])
            for entry in sorted(entries.values(), key=lambda item: item[1]["name"])
        ]
        json.dump(metadata, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return

    if not args.script or not args.output:
        parser.error(
            "--script and --output are required unless --list-generators is used"
        )

    parameters = {}
    if args.params_file:
        try:
            parameters = json.loads(Path(args.params_file).read_text())
        except (OSError, json.JSONDecodeError) as error:
            raise RuntimeError(
                f"Unable to read procedural parameters: {error}"
            ) from error
        if not isinstance(parameters, dict):
            raise RuntimeError("Procedural parameters must be a JSON object")
    cells = []
    if args.cells_file:
        try:
            cells = json.loads(Path(args.cells_file).read_text())
        except (OSError, json.JSONDecodeError) as error:
            raise RuntimeError(
                f"Unable to read the layout cell catalog: {error}"
            ) from error
        if not isinstance(cells, list):
            raise RuntimeError("Layout cell catalog must be a JSON array")
    builder = Layout(
        args.pdk, args.pdk_root, args.db_unit_nm, args.grid, parameters, cells
    )
    with open(args.script, "r", encoding="utf-8") as handle:
        source = handle.read()
    globals_dict = {
        "__name__": "__main__",
        "__file__": args.script,
        "layout": builder,
    }
    try:
        exec(compile(source, args.script, "exec"), globals_dict)
        _apply_dummy_fill_requests(builder)
    except Exception:  # noqa: BLE001 - surface user script failures verbatim
        raise RuntimeError(traceback.format_exc()) from None

    with open(args.output, "w", encoding="utf-8") as handle:
        json.dump(builder.to_dict(), handle, indent=2)
        handle.write("\n")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:  # noqa: BLE001 - report a concise process error
        print(f"procedural layout: {error}", file=sys.stderr)
        sys.exit(1)
