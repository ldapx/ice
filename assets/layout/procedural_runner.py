#!/usr/bin/env python3
"""Execute an ICE procedural layout script and emit generated geometry as JSON.

The user script is executed with a single injected object named ``layout``.
It builds an in-memory description of the generated static layout. ICE parses
the resulting JSON and applies it to the project's layout database.

Coordinates are integer database units (one nanometer by default).
"""

from __future__ import annotations

import argparse
import json
import math
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

    def __init__(self):
        self.layers = {}
        self.shapes = []
        self.ports = []
        self.instances = []

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

    def to_dict(self):
        return {
            "layers": [
                {"name": name, "number": number} for name, number in self.layers.items()
            ],
            "shapes": self.shapes,
            "ports": self.ports,
            "instances": self.instances,
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--script", required=True, help="Procedural layout Python script"
    )
    parser.add_argument("--output", required=True, help="Generated geometry JSON path")
    args = parser.parse_args()

    builder = Layout()
    with open(args.script, "r", encoding="utf-8") as handle:
        source = handle.read()
    globals_dict = {
        "__name__": "__main__",
        "__file__": args.script,
        "layout": builder,
    }
    try:
        exec(compile(source, args.script, "exec"), globals_dict)
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
