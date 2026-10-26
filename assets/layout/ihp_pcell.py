#!/usr/bin/env python3
"""Generate IHP SG13G2 PyCell geometry for the ICE layout editor.

The script intentionally has no gdsfactory dependency. It uses the pure-Python
CNI compatibility layer and PyCell implementations from the IHP Open PDK
repository. ICE invokes this script when a layout-device parameter changes.

Runtime mode:
    ihp_pcell.py --ihp-root /path/to/IHP --module nmos_code --class nmos \
                 --params-file params.json --output geometry.json

Metadata mode:
    ihp_pcell.py --ihp-root /path/to/IHP --module nmos_code --class nmos \
                 --list-params
"""

from __future__ import annotations

import argparse
import importlib
import json
import os
import re
import sys
import types
from pathlib import Path

# Layers used by the PyCell library but missing from the compact CNI tech JSON.
# Values come from ihp/klayout/tech/layers.lyp in the IHP repository.
EXTRA_LAYERS = {
    "EXTBlock": (111, 0),
    "EXTBlock.drawing": (111, 0),
    "RES": (24, 0),
    "RES.drawing": (24, 0),
    "dfpad": (41, 0),
    "dfpad.drawing": (41, 0),
    "dfpad.sbump": (41, 36),
    "HeatRes": (52, 0),
    "HeatRes.drawing": (52, 0),
    "IND.text": (27, 25),
    "LBE.drawing": (157, 0),
    "NoRCX.drawing": (148, 0),
    "Activ.nofill": (1, 23),
    "GatPoly.nofill": (5, 23),
    "Metal1.nofill": (8, 23),
    "Metal2.nofill": (10, 23),
    "Metal3.nofill": (30, 23),
    "Metal4.nofill": (50, 23),
    "Metal5.nofill": (67, 23),
    "TopMetal1.nofill": (126, 23),
    "TopMetal2.nofill": (134, 23),
    "Activ.noqrc": (1, 28),
    "GatPoly.noqrc": (5, 28),
    "Metal1.noqrc": (8, 28),
    "Metal2.noqrc": (10, 28),
    "Metal3.noqrc": (30, 28),
    "Metal4.noqrc": (50, 28),
    "Metal5.noqrc": (67, 28),
    "TopMetal1.noqrc": (126, 28),
    "TopMetal2.noqrc": (134, 28),
}

META_PARAMETERS = {"cdf_version", "Display"}

# Bare numbers entered for geometry parameters are interpreted as micrometers.
# Electrical and dimensionless parameters keep their raw numeric form.
LENGTH_PARAMETERS = {
    "w",
    "ws",
    "l",
    "le",
    "we",
    "d",
    "s",
    "ps",
    "diameter",
    "passEncl",
    "edgeBox",
    "STI",
    "baspolyx",
    "bipwinx",
    "bipwiny",
    "empolyx",
    "empolyy",
    "wfeed",
    "p",
    "pc",
    "Wmin",
    "Lmin",
    "PSmin",
    "Dmin",
    "Smin",
    "CMetY1",
    "CMetY2",
}
BARE_NUMBER = re.compile(r"^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$")


def normalize_override(name: str, value: str, default) -> str:
    if not isinstance(value, str) or not BARE_NUMBER.match(value.strip()):
        return value
    if name not in LENGTH_PARAMETERS:
        return value
    # Inductor "l" is inductance, not length. Its default is in henries.
    if (
        name == "l"
        and isinstance(default, str)
        and default.endswith(("f", "p", "n", "u", "m"))
    ):
        if default.endswith(("p", "f")):
            return value
    return value + "u"


def find_ihp_root(explicit: str | None) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    env = os.environ.get("ICE_IHP_ROOT")
    if env:
        candidates.append(Path(env))
    script_dir = Path(__file__).resolve().parent
    candidates.extend(
        [
            script_dir.parent.parent.parent / "IHP",
            Path.cwd() / "IHP",
            Path.cwd().parent / "IHP",
            Path.home() / "repositories" / "IHP",
        ]
    )
    for candidate in candidates:
        candidate = candidate.expanduser().resolve()
        if (candidate / "cni").is_dir() and (
            candidate / "ihp" / "refs" / "ihp_pycell"
        ).is_dir():
            return candidate
    raise RuntimeError(
        "IHP source root was not found. Set ICE_IHP_ROOT or pass --ihp-root "
        "to a checkout containing cni/ and ihp/refs/ihp_pycell/."
    )


def bootstrap(ihp_root: Path):
    ihp_root = ihp_root.resolve()
    sys.path.insert(0, str(ihp_root))
    import cni  # noqa: F401  (registers SG13_dev)
    from cni.tech import Tech

    pycell_dir = ihp_root / "ihp" / "refs" / "ihp_pycell"
    if "ihp_pycell" not in sys.modules:
        package = types.ModuleType("ihp_pycell")
        package.__path__ = [str(pycell_dir)]  # type: ignore[attr-defined]
        sys.modules["ihp_pycell"] = package

    tech = Tech.get("SG13_dev")
    tech.stream_layers().update(EXTRA_LAYERS)
    return tech


def create_wrapper(tech, module_name: str, class_name: str):
    from cni.dlo import PCellWrapper

    module = importlib.import_module("ihp_pycell." + module_name)
    implementation = getattr(module, class_name)()
    return PCellWrapper(impl=implementation, tech=tech)


def value_text(value) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def parameter_specs(wrapper) -> list[dict[str, str]]:
    specs: list[dict[str, str]] = []
    seen: set[str] = set()
    for declaration in wrapper.param_decls:
        if declaration.name in seen or declaration.name in META_PARAMETERS:
            continue
        seen.add(declaration.name)
        specs.append(
            {
                "name": declaration.name,
                "value": value_text(declaration.default),
                "description": declaration.description or declaration.name,
            }
        )
    return specs


def parse_override(value: str, default):
    if isinstance(default, bool):
        return value.strip().lower() in {"1", "true", "yes", "on", "selected", "all"}
    if isinstance(default, int):
        return int(float(value))
    if isinstance(default, float):
        return float(value)
    return value


def parameter_values(wrapper, overrides: dict[str, str]):
    values = []
    for declaration in wrapper.param_decls:
        if declaration.name in overrides:
            value = normalize_override(
                declaration.name, overrides[declaration.name], declaration.default
            )
        else:
            value = declaration.default
        if isinstance(value, str):
            value = parse_override(value, declaration.default)
        values.append(value)
    return values


def extract_geometry(layout, cell) -> dict:
    import pya

    shapes = []
    min_x = min_y = None
    max_x = max_y = None
    for layer_index in layout.layer_indexes():
        info = layout.get_info(layer_index)
        iterator = cell.begin_shapes_rec(layer_index)
        region = pya.Region()
        found = False
        while not iterator.at_end():
            shape = iterator.shape()
            if shape.is_box():
                region.insert(shape.box)
                found = True
            elif shape.is_polygon():
                region.insert(shape.polygon)
                found = True
            elif shape.is_path():
                try:
                    region.insert(shape.path.polygon())
                    found = True
                except Exception:
                    pass
            # Text shapes are intentionally omitted from the polygon model.
            iterator.next()
        if not found:
            continue
        region.merge()
        for polygon in region.each():
            simple = polygon.to_simple_polygon()
            points = [[point.x, point.y] for point in simple.each_point()]
            if len(points) < 3:
                continue
            for x, y in points:
                min_x = x if min_x is None else min(min_x, x)
                min_y = y if min_y is None else min(min_y, y)
                max_x = x if max_x is None else max(max_x, x)
                max_y = y if max_y is None else max(max_y, y)
            shapes.append(
                {
                    "layer": info.layer,
                    "datatype": info.datatype,
                    "points": points,
                }
            )
    if not shapes or min_x is None:
        raise RuntimeError("the PyCell produced no polygon geometry")
    # Library assets use [min_x, min_y, max_x, max_y], matching KLayout bbox.
    return {
        "bounds": [min_x, min_y, max_x, max_y],
        "shapes": shapes,
    }


def generate(
    ihp_root: Path, module_name: str, class_name: str, overrides: dict[str, str]
) -> dict:
    import pya

    tech = bootstrap(ihp_root)
    wrapper = create_wrapper(tech, module_name, class_name)
    layout = pya.Layout()
    cell = layout.create_cell(class_name)
    wrapper.produce(
        layout=layout,
        layers={},
        parameters=parameter_values(wrapper, overrides),
        cell=cell,
    )
    return extract_geometry(layout, cell)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ihp-root")
    parser.add_argument("--module", required=True)
    parser.add_argument("--class", dest="class_name", required=True)
    parser.add_argument("--params-file")
    parser.add_argument("--output")
    parser.add_argument("--list-params", action="store_true")
    args = parser.parse_args()

    try:
        ihp_root = find_ihp_root(args.ihp_root)
        if args.list_params:
            tech = bootstrap(ihp_root)
            wrapper = create_wrapper(tech, args.module, args.class_name)
            output = {"parameters": parameter_specs(wrapper)}
        else:
            overrides = {}
            if args.params_file:
                overrides = json.loads(Path(args.params_file).read_text())
                if not isinstance(overrides, dict):
                    raise RuntimeError("--params-file must contain a JSON object")
            output = generate(ihp_root, args.module, args.class_name, overrides)
        text = json.dumps(output, separators=(",", ":"))
        if args.output:
            Path(args.output).write_text(text)
        else:
            print(text)
        return 0
    except Exception as error:
        import traceback

        traceback.print_exc()
        print(f"ihp_pcell: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
