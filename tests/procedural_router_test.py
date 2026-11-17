#!/usr/bin/env python3
"""Smoke-test the procedural layout Manhattan autorouter.

Run from the ICE repository root:

    python3 tests/procedural_router_test.py
"""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "assets" / "layout" / "procedural_runner.py"


def load_runner():
    spec = importlib.util.spec_from_file_location("ice_procedural_runner", RUNNER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def overlaps(first, second):
    return not (
        first[0] + first[2] <= second[0]
        or second[0] + second[2] <= first[0]
        or first[1] + first[3] <= second[1]
        or second[1] + second[3] <= first[1]
    )


def shape_rect(shape):
    rectangle = shape._record["rectangle"]
    return (
        rectangle["x"],
        rectangle["y"],
        rectangle["width"],
        rectangle["height"],
    )


def make_device(module, name, x, net="GATE", layer="Metal1", side="right"):
    rectangle = (x, 0, 100, 100)
    return module.DevicePlacement(
        name,
        "nfet",
        "model",
        {},
        [],
        {"G": rectangle},
        {
            "G": {
                "rectangle": rectangle,
                "layer": layer,
                "side": side,
                "direction": "input",
                "net": net,
            }
        },
        {"x": x, "y": 0, "width": 100, "height": 100},
    )


def test_straight_route():
    runner = load_runner()
    layout = runner.Layout()
    layer = layout.layer("Metal1", 1, 0)
    route = layout.route(
        (0, 0, 100, 100),
        (1000, 0, 100, 100),
        layer,
        50,
        net="A",
        start_side="right",
        end_side="left",
    )
    assert route.net == "A"
    assert route.points[0] == (50, 50)
    assert route.points[-1] == (1050, 50)
    assert route.bends == 0
    assert route.shapes
    assert all(shape.net == "A" for shape in route.shapes)


def test_obstacle_route():
    runner = load_runner()
    layout = runner.Layout()
    layer = layout.layer("Metal1", 1, 0)
    obstacle = (400, -500, 200, 1100)
    route = layout.route(
        (0, 0, 100, 100),
        (1000, 0, 100, 100),
        layer,
        50,
        net="A",
        start_side="right",
        end_side="left",
        obstacles=[obstacle],
        clearance=30,
        escape=100,
    )
    assert route.bends >= 2
    for shape in route.shapes:
        assert not overlaps(shape_rect(shape), obstacle), (shape_rect(shape), obstacle)


def test_connect_devices():
    runner = load_runner()
    layout = runner.Layout()
    layout.layer("Metal1", 1, 0)
    first = make_device(runner, "M1", 0, side="right")
    second = make_device(runner, "M2", 3000, side="left")
    layout.devices = [
        {"name": first.name, "bounds": first.bounds},
        {"name": second.name, "bounds": second.bounds},
    ]
    route = layout.connect(first, "G", second, "G")
    assert route.net == "GATE"
    assert route.shapes
    assert all(shape.net == "GATE" for shape in route.shapes)
    assert route.points[0] == (50, 50)
    assert route.points[-1] == (3050, 50)


def test_connect_avoids_other_net_shape():
    runner = load_runner()
    layout = runner.Layout()
    layout.layer("Metal1", 1, 0)
    first = make_device(runner, "M1", 0, side="right")
    second = make_device(runner, "M2", 3000, side="left")
    layout.devices = [
        {"name": first.name, "bounds": first.bounds},
        {"name": second.name, "bounds": second.bounds},
    ]
    obstacle = (1200, -500, 200, 1100)
    layout.rectangle("Metal1", *obstacle, net="OTHER")
    route = layout.connect(first, "G", second, "G")
    assert route.shapes
    for shape in route.shapes:
        assert not overlaps(shape_rect(shape), obstacle), (shape_rect(shape), obstacle)


def test_connect_rejects_different_nets():
    runner = load_runner()
    layout = runner.Layout()
    layout.layer("Metal1", 1, 0)
    first = make_device(runner, "M1", 0, net="GATE", side="right")
    second = make_device(runner, "M2", 3000, net="OUT", side="left")
    layout.devices = [
        {"name": first.name, "bounds": first.bounds},
        {"name": second.name, "bounds": second.bounds},
    ]
    try:
        layout.connect(first, "G", second, "G")
    except ValueError:
        pass
    else:
        raise AssertionError("connecting different nets should fail")


def test_connect_rejects_different_layers():
    runner = load_runner()
    layout = runner.Layout()
    layout.layer("Metal1", 1, 0)
    layout.layer("Metal2", 2, 0)
    first = make_device(runner, "M1", 0, layer="Metal1", side="right")
    second = make_device(runner, "M2", 3000, layer="Metal2", side="left")
    layout.devices = [
        {"name": first.name, "bounds": first.bounds},
        {"name": second.name, "bounds": second.bounds},
    ]
    try:
        layout.connect(first, "G", second, "G")
    except ValueError:
        pass
    else:
        raise AssertionError("connecting different layers should fail without a via")


def make_layer_device(module, name, x, layer, net="GATE", side="right"):
    return make_device(module, name, x, net=net, layer=layer, side=side)


def test_multi_layer_connect_with_explicit_via():
    runner = load_runner()
    layout = runner.Layout()
    layout.layer("Metal1", 1, 0)
    layout.layer("Metal2", 2, 0)
    layout.layer("Via1", 3, 0)
    first = make_layer_device(runner, "M1", 0, "Metal1", side="right")
    second = make_layer_device(runner, "M2", 3000, "Metal2", side="left")
    layout.devices = [
        {"name": first.name, "bounds": first.bounds},
        {"name": second.name, "bounds": second.bounds},
    ]
    route = layout.connect(
        first,
        "G",
        second,
        "G",
        layers=["Metal1", "Metal2"],
        via_layers={("Metal1", "Metal2"): "Via1"},
        via_size=20,
        via_enclosure=10,
    )
    assert route.net == "GATE"
    assert route.via_count >= 1
    assert {segment["layer"] for segment in route.segments} == {"Metal1", "Metal2"}
    assert "Metal1" in route.point_layers and "Metal2" in route.point_layers
    assert any(shape._record["layer"] == "Via1" for shape in route.shapes)
    via_layers = {via["via_layers"][0] for via in route.vias}
    assert via_layers == {"Via1"}


def test_multi_layer_via_inference():
    runner = load_runner()
    layout = runner.Layout()
    layout.layer("met1", 68, 20)
    layout.layer("via", 68, 44)
    layout.layer("met2", 69, 20)
    first = make_layer_device(runner, "M1", 0, "met1", side="right")
    second = make_layer_device(runner, "M2", 3000, "met2", side="left")
    layout.devices = [
        {"name": first.name, "bounds": first.bounds},
        {"name": second.name, "bounds": second.bounds},
    ]
    route = layout.connect(
        first,
        "G",
        second,
        "G",
        layers=["met1", "met2"],
        via_size=15,
        via_enclosure=6,
    )
    assert route.via_count >= 1
    assert route.vias[0]["via_layers"] == ["via"]


def test_multi_layer_two_via_hops():
    runner = load_runner()
    layout = runner.Layout()
    layout.layer("met1", 68, 20)
    layout.layer("via", 68, 44)
    layout.layer("met2", 69, 20)
    layout.layer("via2", 69, 44)
    layout.layer("met3", 70, 20)
    first = make_layer_device(runner, "M1", 0, "met1", side="right")
    second = make_layer_device(runner, "M2", 3000, "met3", side="left")
    layout.devices = [
        {"name": first.name, "bounds": first.bounds},
        {"name": second.name, "bounds": second.bounds},
    ]
    route = layout.connect(
        first,
        "G",
        second,
        "G",
        layers=["met1", "met2", "met3"],
        via_size=15,
        via_enclosure=6,
    )
    assert route.via_count >= 2
    assert {via["via_layers"][0] for via in route.vias} == {"via", "via2"}


def test_package_connectivity_supplies_stack_and_via_sizes():
    runner = load_runner()
    with tempfile.TemporaryDirectory(prefix="ice-connectivity-") as directory:
        root = Path(directory)
        (root / "technology").mkdir()
        (root / "technology" / "connectivity.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "layers": [
                        {"name": "met1", "role": "conductor"},
                        {"name": "met2", "role": "conductor"},
                        {"name": "via", "role": "via"},
                    ],
                    "vias": [
                        {
                            "name": "via",
                            "lower": ["met1"],
                            "upper": ["met2"],
                            "size": 150,
                            "enclosure": 60,
                        }
                    ],
                    "routing": {"stack": ["met1", "met2"]},
                }
            )
        )
        layout = runner.Layout("test-connectivity", root)
        layout.layer("met1", 68)
        layout.layer("met2", 69)
        layout.layer("via", 68, 44)
        assert layout.routing_stack() == ["met1", "met2"]
        route = layout.route(
            (0, 0),
            (4000, 0),
            layers=["met1", "met2"],
            width=200,
            start_layer="met1",
            end_layer="met2",
        )
        assert route.via_count == 1
        assert route.vias[0]["via_layers"] == ["via"]
        assert route.vias[0]["cut_size"] == 150
        assert route.vias[0]["pad_size"] == 270


def test_multi_layer_requires_via_layer():
    runner = load_runner()
    layout = runner.Layout()
    layout.layer("Metal1", 1, 0)
    layout.layer("Metal2", 2, 0)
    first = make_layer_device(runner, "M1", 0, "Metal1", side="right")
    second = make_layer_device(runner, "M2", 3000, "Metal2", side="left")
    layout.devices = [
        {"name": first.name, "bounds": first.bounds},
        {"name": second.name, "bounds": second.bounds},
    ]
    try:
        layout.connect(first, "G", second, "G", layers=["Metal1", "Metal2"])
    except RuntimeError:
        pass
    else:
        raise AssertionError("a missing via layer should fail")


def main() -> int:
    tests = [
        test_straight_route,
        test_obstacle_route,
        test_connect_devices,
        test_connect_avoids_other_net_shape,
        test_connect_rejects_different_nets,
        test_connect_rejects_different_layers,
        test_multi_layer_connect_with_explicit_via,
        test_multi_layer_via_inference,
        test_multi_layer_requires_via_layer,
        test_package_connectivity_supplies_stack_and_via_sizes,
        test_multi_layer_two_via_hops,
    ]
    for test in tests:
        test()
        print(f"[pass] {test.__name__}")
    print("procedural router tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
