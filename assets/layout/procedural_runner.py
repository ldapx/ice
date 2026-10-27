#!/usr/bin/env python3
"""Execute an ICE procedural layout script and emit generated geometry as JSON.

The user script is executed with a single injected object named ``layout``.
It builds an in-memory description of the generated static layout. ICE parses
the resulting JSON and applies it to the project's layout database.

Coordinates are integer database units (one nanometer by default).
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
from pathlib import Path
import sys
import traceback


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


def _shape_points(shape):
    if "rectangle" in shape:
        rectangle = shape["rectangle"]
        x = rectangle["x"]
        y = rectangle["y"]
        width = rectangle["width"]
        height = rectangle["height"]
        return [(x, y), (x + width, y), (x + width, y + height), (x, y + height)]
    return [(point["x"], point["y"]) for point in shape.get("polygon", [])]


class Layer:
    """Handle returned by Layout.layer()."""

    def __init__(self, layout, name):
        self._layout = layout
        self.name = name

    def rectangle(self, x, y, width, height):
        self._layout.rectangle(self.name, x, y, width, height)

    rect = rectangle

    def polygon(self, points):
        self._layout.polygon(self.name, points)

    poly = polygon


class Layout:
    """Builder exposed to procedural layout scripts."""

    def __init__(self, pdk=None, pdk_root=None, db_unit_nm=1.0):
        self.pdk = pdk
        self.pdk_root = pdk_root
        self.db_unit_nm = float(db_unit_nm)
        self.layers = {}
        self.shapes = []
        self.ports = []
        self.instances = []
        self.dummy_fill_requests = []

    def layer(self, name, number=None):
        if not isinstance(name, str) or not name.strip():
            raise ValueError("Layer name must be a non-empty string")
        name = name.strip()
        if number is not None:
            number = _coordinate(number, f"Layer {name} number")
            if number < 0:
                raise ValueError(f"Layer {name} number must not be negative")
        if name not in self.layers or number is not None:
            self.layers[name] = number
        return Layer(self, name)

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
            self.layers[name] = None
        return name

    def rectangle(self, layer, x, y, width, height):
        name = self._layer_name(layer)
        self.shapes.append(
            {
                "layer": name,
                "rectangle": {
                    "x": _coordinate(x, "rectangle x"),
                    "y": _coordinate(y, "rectangle y"),
                    "width": _positive(width, "rectangle width"),
                    "height": _positive(height, "rectangle height"),
                },
            }
        )

    rect = rectangle

    def polygon(self, layer, points):
        name = self._layer_name(layer)
        polygon = []
        for index, point in enumerate(points):
            if isinstance(point, dict):
                x = point.get("x")
                y = point.get("y")
            else:
                try:
                    x, y = point
                except (TypeError, ValueError) as error:
                    raise ValueError(f"polygon point {index} must be (x, y)") from error
            polygon.append(
                {
                    "x": _coordinate(x, f"polygon point {index} x"),
                    "y": _coordinate(y, f"polygon point {index} y"),
                }
            )
        if len(polygon) < 3:
            raise ValueError("polygon must contain at least three points")
        self.shapes.append({"layer": name, "polygon": polygon})

    poly = polygon

    def port(self, name, layer, x, y, width, height):
        if not isinstance(name, str) or not name.strip():
            raise ValueError("Port name must be a non-empty string")
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
        for layer_name, number in self.layers.items():
            if layer_name == name:
                return number
        lowered = name.lower()
        for layer_name, number in self.layers.items():
            if layer_name.lower() == lowered:
                return number
        return None

    def to_dict(self):
        return {
            "layers": [
                {"name": name, "number": number} for name, number in self.layers.items()
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
    args = parser.parse_args()

    builder = Layout(args.pdk, args.pdk_root, args.db_unit_nm)
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
