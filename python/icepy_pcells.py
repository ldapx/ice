"""IHP PCell runtime for :mod:`icepy`.

The upstream IHP PyCell implementations are kept in the IHP checkout and are not
copied into ICE. This module provides the ICE-side runtime:

* bootstrap the IHP pure-Python CNI/PyCell layer,
* evaluate a PCell with parameters,
* transfer the resulting polygons into an ``icepy`` project/layout cell.

The ``main_cli`` entry point is the command-line contract used by the IHP
package's ``layout/device_generator.py``.
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
DEFAULT_PDK = "ihp.sg13g2"

# Bare numbers entered for geometry parameters are interpreted as micrometers.
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


def find_ihp_root(explicit: str | None = None) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    env = os.environ.get("ICE_IHP_ROOT")
    if env:
        candidates.append(Path(env))
    script_dir = Path(__file__).resolve().parent
    candidates.extend(
        [
            script_dir.parent.parent / "IHP",
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
    ihp_root = Path(ihp_root).resolve()
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


def _produce(
    ihp_root: Path, module_name: str, class_name: str, overrides: dict[str, str]
):
    import pya

    tech = bootstrap(ihp_root)
    wrapper = create_wrapper(tech, module_name, class_name)
    layout = pya.Layout()
    cell = layout.create_cell(class_name)
    values = parameter_values(wrapper, overrides)
    wrapper.produce(layout=layout, layers={}, parameters=values, cell=cell)
    return tech, wrapper, values, layout, cell


def _extract_pya_polygons(layout, cell):
    import pya

    polygons = []
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
            iterator.next()
        if not found:
            continue
        region.merge()
        for polygon in region.each():
            simple = polygon.to_simple_polygon()
            points = [(point.x, point.y) for point in simple.each_point()]
            if len(points) >= 3:
                polygons.append(
                    {"number": info.layer, "datatype": info.datatype, "points": points}
                )
    if not polygons:
        raise RuntimeError("the PyCell produced no polygon geometry")
    return polygons


def _layer_names(tech):
    result = {}
    for name, value in tech.stream_layers().items():
        current = result.get(value)
        if current is None:
            result[value] = str(name)
            continue
        # Prefer the compact base name (Metal1) over a datatype-qualified alias
        # (Metal1.drawing) when both describe the same stream layer.
        if "." in current and "." not in str(name):
            result[value] = str(name)
        elif "." not in current and "." in str(name):
            continue
        elif len(str(name)) < len(current):
            result[value] = str(name)
    return result


def _layer_name(reverse, number: int, datatype: int) -> str:
    name = reverse.get((number, datatype))
    if name:
        return str(name)
    return f"Layer {number} (dt {datatype})" if datatype else f"Layer {number}"


def generate_cell(
    project,
    cell_name: str,
    ihp_root: str | Path | None,
    module_name: str,
    class_name: str,
    parameters: dict[str, str] | None = None,
    *,
    pdk: str = DEFAULT_PDK,
    library: bool = True,
    device: str | None = None,
):
    """Generate one IHP PCell into an ``icepy.Project`` layout cell.

    Returns the generated ``icepy.LayoutCell`` snapshot. The project keeps the
    real cell and can save it normally.
    """
    root = find_ihp_root(str(ihp_root) if ihp_root else None)
    tech, wrapper, values, layout, pya_cell = _produce(
        root, module_name, class_name, parameters or {}
    )
    reverse = _layer_names(tech)
    builder = project.create_layout_cell(cell_name, pdk)
    builder.set_library(library)
    builder.set_device(device or class_name)
    for declaration, value in zip(wrapper.param_decls, values):
        if declaration.name in META_PARAMETERS:
            continue
        builder.parameter(declaration.name, value_text(value))
    for polygon in _extract_pya_polygons(layout, pya_cell):
        layer = _layer_name(reverse, polygon["number"], polygon["datatype"])
        builder.layer(layer, polygon["number"])
        points = [make_point(x, y) for x, y in polygon["points"]]
        builder.polygon(layer, points)
    return builder.cell()


def make_point(x, y):
    import icepy

    return icepy.LayoutPoint(int(x), int(y))


def generate_geometry(
    ihp_root: str | Path | None,
    module_name: str,
    class_name: str,
    parameters: dict[str, str] | None = None,
) -> dict:
    """Generate a PCell through ``icepy`` and return the legacy JSON geometry."""
    import icepy

    root = find_ihp_root(str(ihp_root) if ihp_root else None)
    tech = bootstrap(root)
    stream_by_name = {str(name): value for name, value in tech.stream_layers().items()}
    project = icepy.new_project(class_name)
    cell = generate_cell(
        project,
        class_name,
        root,
        module_name,
        class_name,
        parameters,
        pdk=DEFAULT_PDK,
        library=True,
        device=class_name,
    )
    layers_by_id = {layer.id: layer for layer in project.layers}
    shapes = []
    for shape in cell.shapes:
        layer = layers_by_id.get(shape.layer)
        name = layer.name if layer is not None else ""
        number, datatype = stream_by_name.get(name, (layer.number if layer else 0, 0))
        points = [[point.x, point.y] for point in shape.polygon]
        if len(points) < 3:
            continue
        shapes.append(
            {"layer": int(number), "datatype": int(datatype), "points": points}
        )
    if not shapes:
        raise RuntimeError(
            "the PyCell produced no polygon geometry after transfer to icepy"
        )
    bounds = cell.bounds
    if bounds is None:
        raise RuntimeError("the PyCell produced an empty bounding box")
    return {
        "bounds": [
            bounds.x,
            bounds.y,
            bounds.x + bounds.width,
            bounds.y + bounds.height,
        ],
        "shapes": shapes,
    }


def parameter_specs_for(ihp_root: str | Path | None, module_name: str, class_name: str):
    root = find_ihp_root(str(ihp_root) if ihp_root else None)
    tech = bootstrap(root)
    wrapper = create_wrapper(tech, module_name, class_name)
    return parameter_specs(wrapper)


def main_cli(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ihp-root")
    parser.add_argument("--module", required=True)
    parser.add_argument("--class", dest="class_name", required=True)
    parser.add_argument("--params-file")
    parser.add_argument("--output")
    parser.add_argument("--list-params", action="store_true")
    parser.add_argument(
        "--pdk-root", help="Accepted for package-generator compatibility"
    )
    args = parser.parse_args(argv)

    try:
        if args.list_params:
            output = {
                "parameters": parameter_specs_for(
                    args.ihp_root, args.module, args.class_name
                )
            }
        else:
            overrides = {}
            if args.params_file:
                overrides = json.loads(Path(args.params_file).read_text())
                if not isinstance(overrides, dict):
                    raise RuntimeError("--params-file must contain a JSON object")
            output = generate_geometry(
                args.ihp_root, args.module, args.class_name, overrides
            )
        text = json.dumps(output, separators=(",", ":"))
        if args.output:
            Path(args.output).write_text(text)
        else:
            print(text)
        return 0
    except Exception as error:  # noqa: BLE001 - CLI boundary
        print(f"IHP PCell generation failed: {error}", file=sys.stderr)
        return 1


__all__ = [
    "DEFAULT_PDK",
    "find_ihp_root",
    "generate_cell",
    "generate_geometry",
    "main_cli",
    "parameter_specs_for",
]
