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
import importlib.util
import json
import math
from pathlib import Path
import sys
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

    def polygon(self, points, name=None, net=None):
        return self._layout.polygon(self.name, points, name=name, net=net)

    poly = polygon

    def path(self, points, width, name=None, net=None):
        return self._layout.path(self.name, points, width, name=name, net=net)

    def text(self, x, y, text, height=1000, width=None, name=None, net=None):
        return self._layout.text(
            self.name, x, y, text, height=height, width=width, name=name, net=net
        )


class Layout:
    """Builder exposed to procedural layout scripts."""

    def __init__(self, pdk=None, pdk_root=None, db_unit_nm=1.0, grid=1):
        self.pdk = pdk
        self.pdk_root = pdk_root
        self.db_unit_nm = float(db_unit_nm)
        self.grid = max(1, _coordinate(grid, "grid"))
        self.layers = {}
        self.shapes = []
        self.ports = []
        self.instances = []
        self.dummy_fill_requests = []

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

    def polygon(self, layer, points, name=None, net=None):
        name_value = self._layer_name(layer)
        polygon = []
        for index, point in enumerate(points):
            x, y = _point(point, f"polygon point {index}")
            polygon.append({"x": x, "y": y})
        if len(polygon) < 3:
            raise ValueError("polygon must contain at least three points")
        shape = self._add_shape({"layer": name_value, "polygon": polygon})
        if name is not None:
            shape.name = name
        if net is not None:
            shape.net = net
        return shape

    poly = polygon

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
            "shapes": self.shapes,
            "ports": self.ports,
            "instances": self.instances,
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

    builder = Layout(args.pdk, args.pdk_root, args.db_unit_nm, args.grid)
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
