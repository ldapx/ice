#!/usr/bin/env python3
"""Execute an ICE procedural layout script and emit generated geometry as JSON.

The user script is executed with a single injected object named ``layout``.
It builds an in-memory description of the generated static layout. ICE parses
the resulting JSON and applies it to the project's layout database.

Coordinates are integer database units (one nanometer by default). Use
``layout.nm(...)``, ``layout.um(...)``, ``layout.db(...)``, and
``layout.snap(...)`` to work in physical units.
"""

from __future__ import annotations

import argparse
import copy
import heapq
import importlib.util
import json
import math
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import traceback


SCHEMA_VERSION = 2


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
    """Handle for a shape added through the layout builder."""

    def __init__(self, layout, record):
        self._layout = layout
        self._record = record

    @property
    def name(self):
        return self._record.get("name", "")

    @name.setter
    def name(self, value):
        if value is None or str(value).strip() == "":
            self._record.pop("name", None)
            return
        self._record["name"] = str(value).strip()

    @property
    def net(self):
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
    object keeps the absolute pin rectangles and the device metadata so a
    script can route to the device and LVS can recognize it.
    """

    def __init__(self, name, device, model, parameters, shapes, ports, port_info, bounds):
        self.name = name
        self.device = device
        self.model = model
        self.parameters = parameters
        self.shapes = shapes
        self.ports = ports
        self.port_info = port_info
        self.bounds = bounds

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


class Route:
    """Handle returned by :meth:`Layout.route` and :meth:`Layout.connect`."""

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
    return any(left < x < right and top < y < bottom
               for left, top, right, bottom in inflated)


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
        "no clear escape from the pin at ({}, {}) size {}x{}: every side is blocked by "
        "metal within the route clearance. Reduce the route width/clearance, move the "
        "device, or escape from a different pin.".format(x, y, width, height)
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
                first[0] == second[0] == point[0]
                or first[1] == second[1] == point[1]
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
                first[0] == second[0] == point[0]
                or first[1] == second[1] == point[1]
            ):
                simplified[-1] = point
                continue
        simplified.append(point)
    return simplified


class Layer:
    """Handle returned by Layout.layer()."""

    def __init__(self, layout, name, number=None, datatype=None):
        self._layout = layout
        self.name = name
        self.number = number
        self.datatype = datatype

    def rectangle(self, x, y, width, height, name=None, net=None):
        return self._layout.rectangle(
            self.name, x, y, width, height, name=name, net=net
        )

    rect = rectangle

    def polygon(self, points, holes=None, name=None, net=None):
        return self._layout.polygon(self.name, points, holes=holes, name=name, net=net)

    poly = polygon

    def path(self, points, width, name=None, net=None):
        return self._layout.path(self.name, points, width, name=name, net=net)

    def text(self, x, y, text, height=1000, width=None, name=None, net=None):
        return self._layout.text(
            self.name, x, y, text, height=height, width=width, name=name, net=net
        )


class Layout:
    """Builder exposed to procedural layout scripts."""

    def __init__(self, pdk=None, pdk_root=None, db_unit_nm=1.0, grid=1, parameters=None):
        self.pdk = pdk
        self.pdk_root = pdk_root
        self.db_unit_nm = float(db_unit_nm)
        self.grid = max(1, _coordinate(grid, "grid"))
        self.params = {
            str(name): "" if value is None else str(value)
            for name, value in (parameters or {}).items()
        }
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
        """Register or resolve a layer.

        When ``number``/``datatype`` are omitted, ICE resolves the layer from
        the active PDK package or the project's existing layers. Explicit
        values win when no matching PDK/project layer exists.
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
            if "boolean" not in record and "polygon" not in record and "rectangle" not in record:
                raise ValueError("Boolean operand has no filled geometry")
            operand._record["boolean_operand"] = True
            record["boolean_operand"] = True
            return record
        if not isinstance(operand, (list, tuple)):
            raise ValueError("Boolean operands must be shapes or point lists")
        if len(operand) == 4 and all(isinstance(value, (int, float)) for value in operand):
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
        and ``xor``. Operands are consumed by the operation.
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
            raise ValueError("Boolean operation must be union, intersection, difference, or xor")
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
        return self.boolean("union", layer, *operands, name=name, net=net)

    def intersection(self, layer, *operands, name=None, net=None):
        return self.boolean("intersection", layer, *operands, name=name, net=net)

    def subtract(self, layer, *operands, name=None, net=None):
        return self.boolean("difference", layer, *operands, name=name, net=net)

    def xor(self, layer, *operands, name=None, net=None):
        return self.boolean("xor", layer, *operands, name=name, net=net)

    def path(self, layer, points, width, name=None, net=None):
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
        """Convenience helper for a stack of square/rectangular via shapes."""
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
        """Place a rectangular array of an existing layout cell."""
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

    def instance(self, cell, x, y, orientation="r0", name=None):
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
        self.instances.append(
            {
                "cell": cell.strip(),
                "name": name.strip(),
                "x": _coordinate(x, "instance x"),
                "y": _coordinate(y, "instance y"),
                "orientation": orientation,
            }
        )

    def parameter(self, name, default="", description=""):
        """Declare a user-editable parameter and return its current value."""
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
            raise RuntimeError("Device placement requires an installed PDK package root")
        root = Path(self.pdk_root)
        index_path = root / "layout" / "devices" / "index.json"
        if not index_path.is_file():
            pdk = self.pdk or "unknown"
            raise RuntimeError(f"PDK '{pdk}' does not provide {index_path}")
        try:
            library = json.loads(index_path.read_text())
        except (OSError, json.JSONDecodeError) as error:
            raise RuntimeError(f"Unable to read device library {index_path}: {error}") from error
        self._device_layers = {}
        for layer in library.get("layers", []):
            try:
                number = int(layer["number"])
                datatype = int(layer.get("datatype", 0))
            except (KeyError, TypeError, ValueError):
                continue
            self._device_layers[(number, datatype)] = str(layer.get("name", f"Layer {number}"))
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
                raise RuntimeError(f"Device '{cell.get('name', '')}' has no generator or layout file")
            path = Path(self.pdk_root) / "layout" / "devices" / str(layout_path)
            try:
                document = json.loads(path.read_text())
                return document["device"]
            except (OSError, KeyError, json.JSONDecodeError) as error:
                raise RuntimeError(f"Unable to read device geometry {path}: {error}") from error

        key = (str(generator.get("module", "")), str(generator.get("class", "")),
               tuple(sorted((str(k), str(v)) for k, v in parameters.items())))
        cached = self._device_geometry_cache.get(key)
        if cached is not None:
            return cached
        generator_path = Path(self.pdk_root) / "layout" / "devices" / "generator.py"
        if not generator_path.is_file():
            raise RuntimeError(f"Device generator is missing: {generator_path}")
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
                raise RuntimeError(f"Unable to run device generator: {error}") from error
            if completed.returncode != 0:
                details = (completed.stderr or completed.stdout or "").strip()
                raise RuntimeError(
                    f"Device generator for '{cell.get('name', '')}' failed: {details or completed.returncode}"
                )
            try:
                geometry = json.loads(output_path.read_text())
            except (OSError, json.JSONDecodeError) as error:
                raise RuntimeError(f"Unable to read generated device geometry: {error}") from error
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
            return {"left": "bottom", "right": "top", "top": "left", "bottom": "right"}.get(side, side)
        if orientation == "r180":
            return {"left": "right", "right": "left", "top": "bottom", "bottom": "top"}.get(side, side)
        if orientation == "r270":
            return {"left": "top", "right": "bottom", "top": "right", "bottom": "left"}.get(side, side)
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
        multiple copies can coexist. ``nets`` maps device pin names to layout
        net names, for example ``nets={"G": "GATE", "S": "VSS"}``.
        """
        cell = self._device_cell(device_name)
        orientation = str(orientation)
        if orientation not in {"r0", "r90", "r180", "r270", "mx", "my"}:
            raise ValueError("Device orientation must be r0, r90, r180, r270, mx, or my")
        defaults = {
            str(parameter.get("name", "")): str(parameter.get("value", ""))
            for parameter in cell.get("parameters", [])
        }
        values = dict(defaults)
        for key, value in parameters.items():
            values[str(key)] = "" if value is None else str(value)
        geometry = self._device_geometry(cell, values)
        self._device_count += 1
        instance_name = str(name).strip() if name is not None else f"device_{self._device_count}"
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
                        raise RuntimeError("Device shape point must be (x, y)") from error
                tx, ty = self._device_transform(
                    _coordinate(px, "device point x"),
                    _coordinate(py, "device point y"),
                    orientation,
                )
                points.append({"x": tx + origin_x, "y": ty + origin_y})
            if len(points) < 3:
                continue
            layer_name = self._device_layer_name(source.get("layer", 0), source.get("datatype", 0))
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
        port_info = {}
        for source in geometry.get("ports", []):
            rect = source.get("rectangle", {})
            corners = []
            for px, py in (
                (rect.get("x", 0), rect.get("y", 0)),
                (rect.get("x", 0) + rect.get("width", 0), rect.get("y", 0)),
                (rect.get("x", 0) + rect.get("width", 0), rect.get("y", 0) + rect.get("height", 0)),
                (rect.get("x", 0), rect.get("y", 0) + rect.get("height", 0)),
            ):
                tx, ty = self._device_transform(
                    _coordinate(px, "device port x"), _coordinate(py, "device port y"), orientation
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
            layer_name = self._device_layer_name(source.get("layer", 0), source.get("datatype", 0))
            side = self._device_side(str(source.get("side", "left")), orientation)
            direction = str(source.get("direction", "inout"))
            self.ports.append(
                {
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
            )
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
                    "rectangle": rectangle,
                }
            )
        # Propagate pin nets to the device geometry that carries each pin.
        # This lets the router treat other terminals as obstacles while
        # allowing a connection to travel along its own pin metal.
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
                {"name": str(key), "value": str(value)}
                for key, value in values.items()
            ],
            "bounds": bounds,
            "ports": port_records,
            "port_info": port_info,
        }
        self.devices.append(device_record)
        return DevicePlacement(
            instance_name,
            str(cell.get("name", device_name)),
            model,
            dict(values),
            shapes,
            port_rectangles,
            port_info,
            bounds,
        )

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
        if not isinstance(document, dict) or int(document.get("schema_version", 0)) != 1:
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
        routing_stack = [
            str(name) for name in stack or [] if str(name).strip()
        ] if isinstance(stack, list) else []
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
        :meth:`route` or :meth:`connect`.
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
            matches = [name for name in available_names if _connectivity_key(name) == key]
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
            exact = [candidate for candidate in via_candidates if candidate[1] == lower_index]
            if exact:
                exact.sort(key=lambda item: (0 if "topvia" not in item[0].lower() else 1, item[0]))
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
        """Route one Manhattan connection between two points or port rectangles.

        Use ``layer`` for a single-layer route, or ``layers=[...]`` to route
        through a vertical layer stack. When both are omitted, the PDK
        package's declared routing stack is used. In a multi-layer route the
        router may insert via stacks between adjacent layers. ``via_layers``
        maps a ``(lower, upper)`` pair to one cut layer or a list of cut layers;
        when omitted, the package connectivity document is consulted first and
        common names such as ``via``/``via2``, ``Via1``/``Via2`` and
        ``Metal1``/``Metal2`` are used as a fallback.
        """
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
        via_size = self.um(0.15) if via_size is None else _positive(via_size, "via size")
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
                        f"pin layer '{info['layer']}' does not match route layer '{name}'"
                    )
                return name
            if info and info.get("layer"):
                if info["layer"] in layer_names:
                    return info["layer"]
                raise ValueError(
                    f"pin layer '{info['layer']}' is not in the route layer stack"
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
                if not via_enclosure_explicit and declared_via.get("enclosure") is not None:
                    pair_enclosure = _coordinate(self.nm(declared_via["enclosure"]), "via enclosure")
            pair_pad = max(width, pair_size + 2 * pair_enclosure)
            metal_inflate = clearance + pair_pad // 2
            cut_inflate = clearance + pair_size // 2
            lower_obstacles = _route_inflate(
                obstacles_by_layer[lower], metal_inflate
            )
            upper_obstacles = _route_inflate(
                obstacles_by_layer[upper], metal_inflate
            )
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
                return not any(_route_inside_obstacle(x, y, obstacle) for obstacle in co)

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
                for (lower_index, upper_index) in via_options:
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
        return Route(
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
        """Route directly between two pins of placed package devices.

        ``device_a`` and ``device_b`` are the ``DevicePlacement`` objects
        returned by :meth:`device`. For a single-layer connection, ``layer``
        is optional when both pins already share a layer. For pins on
        different layers, pass ``layers=[...]`` in vertical stack order and,
        if the package does not expose inferable via names, a ``via_layers``
        mapping.
        """
        if not isinstance(device_a, DevicePlacement) or not isinstance(
            device_b, DevicePlacement
        ):
            raise TypeError(
                "connect expects DevicePlacement objects from layout.device()"
            )
        info_a = device_a.port_info.get(str(pin_a))
        info_b = device_b.port_info.get(str(pin_b))
        if info_a is None:
            raise ValueError(f"device '{device_a.name}' has no pin '{pin_a}'")
        if info_b is None:
            raise ValueError(f"device '{device_b.name}' has no pin '{pin_b}'")
        layer_a = info_a.get("layer")
        layer_b = info_b.get("layer")
        if layers is None:
            if layer is None:
                if layer_a and layer_b and layer_a != layer_b:
                    raise ValueError(
                        "connect pins are on different layers; pass layers=[...] "
                        "and optionally via_layers=..."
                    )
                layer = layer_a or layer_b
            if layer is None:
                raise ValueError(
                    "connect requires a layer for pins without layer metadata"
                )
            layer_name = self._layer_name(layer)
            for label, info in (("start", info_a), ("end", info_b)):
                pin_layer = info.get("layer")
                if pin_layer and pin_layer != layer_name:
                    raise ValueError(
                        f"{label} pin is on {pin_layer}, not {layer_name}; "
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
                raise ValueError("connect pins need layer metadata for a layer stack")
            if layer_a not in layer_names or layer_b not in layer_names:
                raise ValueError(
                    "both pin layers must be included in the layers stack"
                )
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
                raise ValueError(
                    f"start pin belongs to net '{start_net}', not '{net}'"
                )
            if end_net and end_net != net:
                raise ValueError(f"end pin belongs to net '{end_net}', not '{net}'")
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
        """Return ``(min_x, min_y, max_x, max_y)`` for the selected geometry."""
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
        """Return ``value`` as integer database units."""
        return _coordinate(value, "database value")

    def nm(self, value):
        """Convert nanometers to integer database units."""
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
        """Convert ``2.5u``/``150n``/``1mm`` or a bare DB-unit number."""
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
        return {
            "format": "ice-procedural-layout",
            "schema_version": SCHEMA_VERSION,
            "db_unit_nm": self.db_unit_nm,
            "grid": self.grid,
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
    parser.add_argument(
        "--script", required=True, help="Procedural layout Python script"
    )
    parser.add_argument("--output", required=True, help="Generated geometry JSON path")
    parser.add_argument("--params-file", help="Optional procedural parameter values (JSON object)")
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

    parameters = {}
    if args.params_file:
        try:
            parameters = json.loads(Path(args.params_file).read_text())
        except (OSError, json.JSONDecodeError) as error:
            raise RuntimeError(f"Unable to read procedural parameters: {error}") from error
        if not isinstance(parameters, dict):
            raise RuntimeError("Procedural parameters must be a JSON object")
    builder = Layout(args.pdk, args.pdk_root, args.db_unit_nm, args.grid, parameters)
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
