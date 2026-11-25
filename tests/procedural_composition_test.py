#!/usr/bin/env python3
"""Smoke-test procedural layout composition, constraints, and generators.

Run from the ICE repository root:

    python3 tests/procedural_composition_test.py
"""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "assets" / "layout" / "procedural_runner.py"


def load_runner():
    spec = importlib.util.spec_from_file_location(
        "ice_procedural_runner_compose", RUNNER
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def install_fake_device(layout):
    layout._device_cell = lambda name: {
        "name": str(name),
        "model": str(name),
        "parameters": [{"name": "w", "value": "1u"}],
    }
    layout._device_geometry = lambda cell, values: {
        "shapes": [
            {
                "layer": 1,
                "datatype": 0,
                "points": [
                    {"x": 0, "y": 0},
                    {"x": 100, "y": 0},
                    {"x": 100, "y": 200},
                    {"x": 0, "y": 200},
                ],
            }
        ],
        "ports": [
            {
                "name": "G",
                "layer": 1,
                "datatype": 0,
                "rectangle": {"x": 0, "y": 50, "width": 20, "height": 100},
                "side": "left",
                "direction": "input",
            }
        ],
    }
    layout._device_layer_name = lambda number, datatype: "Metal1"
    layout._device_side = lambda side, orientation: side


def rectangle(layout, x, y, width, height, name=None, net=None):
    return layout.rectangle("Metal1", x, y, width, height, name=name, net=net)


def test_group_row_and_column():
    runner = load_runner()
    layout = runner.Layout()
    pair = layout.group("pair")
    first = rectangle(pair, 0, 0, 100, 50, "first")
    second = rectangle(pair, 0, 0, 100, 80, "second")

    row = layout.row(gap=0, align="bottom")
    row.add(first)
    row.add(second)
    assert row.bounds == (0, 0, 200, 80), row.bounds
    assert (first.bounds[0], second.bounds[0]) == (0, 100)

    column = layout.column(gap=10, align="left")
    column.add(row)
    column.add(rectangle(layout, 0, 0, 40, 20))
    assert column.bounds == (0, 0, 200, 110), column.bounds

    layout.add(column, x=100, y=200)
    assert layout.bounds() == (100, 200, 300, 310), layout.bounds()


def test_row_and_column_accept_immediate_items():
    runner = load_runner()
    layout = runner.Layout()
    first = rectangle(layout, 0, 0, 100, 50, "first")
    second = rectangle(layout, 0, 0, 100, 80, "second")
    row = layout.row([first, second], gap=0, align="bottom")
    assert row.bounds == (0, 0, 200, 80), row.bounds
    third = rectangle(layout, 0, 0, 40, 20, "third")
    column = layout.column([row, third], gap=10, align="left")
    assert column.bounds == (0, 0, 200, 110), column.bounds


def test_nested_composition_and_measurement():
    runner = load_runner()
    layout = runner.Layout()
    a = rectangle(layout, 0, 0, 100, 100, "a")
    b = rectangle(layout, 0, 0, 50, 200, "b")
    row = layout.row(gap=20, align="center")
    row.add(a)
    row.add(b)
    assert row.bounds == (0, 0, 170, 200)
    assert layout.clearance(a, b) == 20
    assert not layout.overlaps(a, b)
    assert layout.distance(a, b, axis="x") == -95
    measured = layout.measure(row)
    assert measured["bounds"] == (0, 0, 170, 200)
    assert measured["area"] == 170 * 200

    frame = layout.group("frame")
    rectangle(frame, -10, -10, 200, 230)
    assert frame.bounds == (-10, -10, 190, 220)
    assert layout.contains(frame, row)
    assert not layout.contains(row, frame)


def test_mirror_anchor_and_pin():
    runner = load_runner()
    layout = runner.Layout()
    shape = rectangle(layout, 10, 20, 40, 60, "shape")
    assert shape.anchor("sw") == (10, 20)
    assert shape.anchor("center") == (30, 50)
    shape.mirror("y")
    assert shape.bounds == (10, 20, 50, 80)
    shape.move(5, -5)
    assert shape.bounds == (15, 15, 55, 75)

    install_fake_device(layout)
    device = layout.device("nfet", name="M1", nets={"G": "IN"})
    assert device.pin("G") == (0, 50, 20, 100)
    device.mirror("y")
    assert device.pin("G") == (80, 50, 20, 100)
    device.move(5, 5)
    assert device.pin("G") == (85, 55, 20, 100)


def test_grid_stack_and_repeat():
    runner = load_runner()
    layout = runner.Layout()
    grid = layout.grid(columns=2, x_gap=10, y_gap=10, align="center")
    for index in range(4):
        grid.add(rectangle(layout, 0, 0, 20 + index, 10))
    assert grid.bounds == (0, 0, 55, 30), grid.bounds

    stack = layout.stack()
    stack.add(rectangle(layout, 0, 0, 100, 50))
    stack.add(rectangle(layout, 20, 10, 100, 50))
    assert stack.bounds == (0, 0, 100, 50), stack.bounds

    unit = layout.group("unit")
    rectangle(unit, 0, 0, 10, 10)
    unit.port("P", "Metal1", 0, 0, 10, 10, net="N")
    repeated = layout.repeat(unit, count=3, pitch="abut", mirror=True, alternate=True)
    assert repeated.bounds == (0, 0, 30, 10), repeated.bounds
    assert [port["name"] for port in repeated.ports] == ["P", "P_2", "P_3"]
    assert len(repeated.shapes) == 3


def test_constraints():
    runner = load_runner()
    layout = runner.Layout()
    first = rectangle(layout, 0, 0, 10, 10)
    second = rectangle(layout, 40, 20, 10, 30)
    third = rectangle(layout, 80, 5, 10, 20)

    layout.align(first, second, third, line="h_center")
    centers = [
        (item.bounds[1] + item.bounds[3]) // 2 for item in (first, second, third)
    ]
    assert len(set(centers)) == 1, centers

    layout.order([first, second, third], direction="left_to_right", abut=True)
    assert first.bounds[2] == second.bounds[0]
    assert second.bounds[2] == third.bounds[0]

    left = rectangle(layout, 0, 0, 20, 20)
    right = rectangle(layout, 100, 0, 20, 20)
    layout.symmetric([[left, right]], axis="vertical")
    left_center = (left.bounds[0] + left.bounds[2]) // 2
    right_center = (right.bounds[0] + right.bounds[2]) // 2
    assert (left_center, right_center) == (10, 110)

    layout.constraint(
        "Spread", items=[left, right], direction="horizontal", distance=10
    )
    assert layout.clearance(left, right) >= 10

    try:
        layout.boundary(max_width=10, max_height=10)
    except ValueError as error:
        assert "Boundary" in str(error)
    else:
        raise AssertionError("boundary should reject oversized geometry")

    grouped = layout.constraint("Group", items=[left, right], name="pair")
    assert grouped.bounds == layout.extents(left, right)
    assert layout.contains(grouped, left)

    json.dumps(layout.to_dict())

    try:
        layout.constraint("Align", items=[first, 42], line="h_center")
    except TypeError as error:
        assert "measure" in str(error)
    else:
        raise AssertionError("invalid constraint item should fail")


def test_inline_and_builtin_generators():
    runner = load_runner()
    layout = runner.Layout()

    @layout.generator(
        name="pair",
        description="Two adjacent rectangles",
        parameters={"width": 10},
        ports=["P"],
    )
    def pair(ctx, width):
        row = ctx.row(gap=0)
        left = ctx.rectangle("Metal1", 0, 0, width, 10)
        right = ctx.rectangle("Metal1", 0, 0, width, 10)
        row.add(left)
        row.add(right)
        row.port("P", "Metal1", 0, 0, width, 10)
        return row

    generated = layout.generate("pair", width=20)
    assert generated.bounds == (0, 0, 40, 10), generated.bounds
    assert generated.ports[0]["name"] == "P"
    assert layout.to_dict()["generator_metadata"][0]["name"] == "pair"

    rail = layout.generate("rail", layer="Metal1", width=100, height=20)
    assert rail.bounds == (0, 0, 100, 20)
    ring = layout.generate(
        "guard_ring", layer="Metal1", x=0, y=0, width=100, height=50, ring=10
    )
    assert ring.bounds == (-10, -10, 110, 60)
    vias = layout.generate(
        "via_stack", layers=["Metal1", "Via1", "Metal2"], x=0, y=0, size=10
    )
    assert len(vias.shapes) == 3
    bundle = layout.generate(
        "route_bundle", layer="Metal1", count=2, pitch=20, length=100, width=5
    )
    assert len(bundle.shapes) == 2
    assert bundle.bounds[2] - bundle.bounds[0] >= 100
    assert bundle.bounds[3] - bundle.bounds[1] < 100


def test_package_generator_registry():
    runner = load_runner()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        generators = root / "layout" / "generators"
        generators.mkdir(parents=True)
        (generators / "analog.py").write_text(
            "def double(ctx, width=10):\n"
            "    row = ctx.row(gap=0)\n"
            "    row.add(ctx.rectangle('Metal1', 0, 0, width, 10))\n"
            "    row.add(ctx.rectangle('Metal1', 0, 0, width, 10))\n"
            "    return row\n"
        )
        (generators / "index.json").write_text(
            json.dumps(
                [
                    {
                        "name": "double",
                        "module": "analog.py",
                        "function": "double",
                        "description": "Double rectangle",
                        "version": 2,
                        "parameters": {"width": 5},
                    }
                ]
            )
        )
        layout = runner.Layout(pdk_root=str(root))
        generated = layout.generate("double", width=20)
        assert generated.bounds == (0, 0, 40, 10)
        metadata = layout.to_dict()["generator_metadata"][0]
        assert metadata["name"] == "double"
        assert metadata["source"] == "package"
        assert metadata["version"] == 2


def test_generator_discovery_command():
    output = subprocess.check_output(
        [sys.executable, str(RUNNER), "--list-generators"], text=True
    )
    entries = json.loads(output)
    names = {entry["name"] for entry in entries}
    assert {
        "mos",
        "mos_pair",
        "resistor_bank",
        "cap_array",
        "via_stack",
        "guard_ring",
        "rail",
        "route_bundle",
    } <= names
    for entry in entries:
        assert entry["source"] == "builtin"
        assert entry["description"]


def test_layout_plan_gate_and_review():
    runner = load_runner()
    layout = runner.Layout()
    install_fake_device(layout)
    layout.plan(
        devices=["M1", "M2"],
        rows=1,
        target_utilization=0.0,
        max_gap_um=10.0,
        require_composition=True,
    )
    first = layout.device("nfet", name="M1")
    second = layout.device("nfet", name="M2")
    row = layout.row(gap=0)
    row.add(first)
    row.add(second)
    layout.add(row)

    try:
        layout.route((0, 5000, 10, 10), (100, 5000, 10, 10), layer="Metal1", width=5)
    except runner.LayoutQualityError as error:
        assert "compaction gate" in str(error)
    else:
        raise AssertionError("routing must be blocked before the compaction gate")

    quality = layout.finish_placement()
    assert quality["plan_passed"]
    assert quality["composition_used"]
    assert quality["row_count"] == 1
    route = layout.route(
        (0, 5000, 10, 10), (100, 5000, 10, 10), layer="Metal1", width=5
    )
    assert route.bounds is not None

    review = layout.review(require_plan=True)
    assert review["passed"]
    output = layout.to_dict()
    assert output["layout_quality"]["plan_passed"]
    assert output["layout_plan"]["passed"]
    assert output["layout_review"]["passed"]


def test_layout_plan_gate_rejects_low_utilization():
    runner = load_runner()
    layout = runner.Layout()
    install_fake_device(layout)
    layout.plan(
        devices=["M1", "M2"],
        rows=1,
        target_utilization=0.95,
        max_gap_um=10.0,
    )
    first = layout.device("nfet", name="M1")
    second = layout.device("nfet", name="M2")
    row = layout.row(gap=1000)
    row.add(first)
    row.add(second)
    layout.add(row)
    try:
        layout.finish_placement()
    except runner.LayoutQualityError as error:
        assert "below target" in str(error)
    else:
        raise AssertionError("the compaction gate should reject low utilization")
    review = layout.review(min_utilization=0.95)
    assert not review["passed"]
    assert any("below review target" in issue for issue in review["issues"])


def test_report_groups_rows_before_measuring_gaps():
    runner = load_runner()
    layout = runner.Layout()
    install_fake_device(layout)
    first = layout.device("nfet", name="M1")
    second = layout.device("nfet", name="M2")
    third = layout.device("nfet", name="M3")
    second.move(500, 0)
    third.move(0, 1000)
    report = layout.report()
    assert report["row_count"] == 2
    assert report["max_horizontal_gap_um"] == 0.4
    assert report["large_gaps"] == []
    assert all(
        not (gap["left"] == "M1" and gap["right"] == "M3")
        for gap in report["large_gaps"]
    )


def test_layout_instance_composition():
    runner = load_runner()
    cells = [
        {
            "name": "digital",
            "bounds": {"x": 0, "y": 0, "width": 1000, "height": 600},
            "ports": [
                {
                    "name": "clk",
                    "layer": "Metal1",
                    "rectangle": {"x": 100, "y": 100, "width": 20, "height": 80},
                    "side": "left",
                    "direction": "input",
                    "net": "clk",
                }
            ],
        }
    ]
    layout = runner.Layout(cells=cells)
    pad = rectangle(layout, 0, 0, 100, 100, "pad")
    block = layout.instance("DIGITAL", 0, 0, name="DIGITAL")
    assert isinstance(block, runner.LayoutInstancePlacement)
    assert block.cell == "digital"
    assert block.bounds == {"x": 0, "y": 0, "width": 1000, "height": 600}
    assert block.pin("clk") == (100, 100, 20, 80)

    row = layout.row(gap=0)
    row.add(pad)
    row.add(block)
    layout.add(row)
    assert layout.instances[0]["x"] == 100
    assert layout.bounds() == (0, 0, 1100, 600)

    report = layout.report()
    assert report["instance_count"] == 1
    assert report["instance_area_um2"] > 0
    assert report["utilization"] > 0.8
    assert report["placement_style"] == "composed"

    direct = runner.Layout(cells=cells)
    moved = direct.instance("digital", 0, 0, name="D")
    direct.add(moved, x=200, y=300)
    assert direct.instances[0]["x"] == 200
    assert direct.instances[0]["y"] == 300
    moved.move(10, 20)
    assert direct.instances[0]["x"] == 210
    assert direct.instances[0]["y"] == 320

    rotated = runner.Layout(cells=cells).instance("digital", 0, 0, orientation="r90")
    assert rotated.bounds == {"x": -600, "y": 0, "width": 600, "height": 1000}
    assert rotated.pin("clk") == (-180, 100, 80, 20)

    mirrored = runner.Layout(cells=cells).instance("digital", 0, 0, name="M")
    mirrored.mirror("y")
    assert mirrored.orientation == "my"
    assert mirrored.bounds == {"x": 0, "y": 0, "width": 1000, "height": 600}
    assert mirrored.pin("clk") == (880, 100, 20, 80)

    planned = runner.Layout(cells=cells)
    planned.plan(
        devices=0,
        target_utilization=0.9,
        max_gap_um=10000,
    )
    planned.instance("digital", 0, 0, name="D")
    quality = planned.finish_placement()
    assert quality["plan_passed"]
    assert quality["instance_count"] == 1
    assert quality["utilization"] == 1.0

    try:
        runner.Layout(cells=cells).instance("missing", 0, 0)
    except ValueError as error:
        assert "Unknown layout cell" in str(error)
    else:
        raise AssertionError("unknown instance cells must be rejected")


def test_layout_instance_connect():
    runner = load_runner()
    cells = [
        {
            "name": "digital",
            "bounds": {"x": 0, "y": 0, "width": 1000, "height": 600},
            "ports": [
                {
                    "name": "clk",
                    "layer": "Metal1",
                    "rectangle": {"x": 100, "y": 100, "width": 20, "height": 80},
                    "side": "left",
                    "direction": "input",
                    "net": "clk",
                }
            ],
        }
    ]
    layout = runner.Layout(cells=cells)
    first = layout.instance("digital", 0, 0, name="A")
    second = layout.instance("digital", 2000, 0, name="B")
    route = layout.connect(first, "clk", second, "clk", layer="Metal1")
    assert route.net == "clk"
    assert route.bounds is not None


def test_device_composition():
    runner = load_runner()
    layout = runner.Layout()
    install_fake_device(layout)
    first = layout.device("nfet", name="M1", nets={"G": "IN"})
    second = layout.device("nfet", name="M2", nets={"G": "IN"})
    row = layout.row(gap=0)
    row.add(first)
    row.add(second)
    assert row.bounds == (0, 0, 200, 200)
    assert first.pin("G") == (0, 50, 20, 100)
    assert second.pin("G") == (100, 50, 20, 100)
    first.mirror("y")
    assert first.pin("G") == (80, 50, 20, 100)
    first.mirror("y")
    layout.add(row)
    assert layout.bounds() == (0, 0, 200, 200)
    assert sorted(port["name"] for port in layout.ports) == ["M1.G", "M2.G"]
    assert len(layout.devices) == 2
    third = layout.device("nfet", name="M3")
    layout.move(5, 5)
    assert third.pin("G") == (5, 55, 20, 100)
    report = layout.report(gap_threshold=1)
    assert report["device_count"] == 3
    assert report["shape_count"] > 0
    assert report["utilization"] > 0
    assert "warnings" in report

    sparse = runner.Layout()
    install_fake_device(sparse)
    sparse_left = sparse.device("nfet", name="S1")
    sparse_right = sparse.device("nfet", name="S2")
    sparse_right.move(1000, 0)
    sparse_report = sparse.report()
    assert sparse_report["max_horizontal_gap_um"] > 0
    assert sparse_report["warnings"]


def test_device_layers_keep_real_stream_numbers():
    runner = load_runner()
    layout = runner.Layout()
    layer_names = {
        (40, 0): "Substrate",
        (5, 2): "GatPoly.pin",
        (51, 0): "HeatTrans",
    }
    layout._device_cell = lambda name: {
        "name": str(name),
        "model": str(name),
        "parameters": [],
    }
    layout._device_geometry = lambda cell, values: {
        "shapes": [
            {
                "layer": 40,
                "datatype": 0,
                "points": [
                    {"x": 0, "y": 0},
                    {"x": 100, "y": 0},
                    {"x": 100, "y": 100},
                ],
            },
            {
                "layer": 5,
                "datatype": 2,
                "points": [
                    {"x": 0, "y": 0},
                    {"x": 20, "y": 0},
                    {"x": 20, "y": 20},
                ],
            },
            {
                "layer": 51,
                "datatype": 0,
                "points": [
                    {"x": 0, "y": 0},
                    {"x": 30, "y": 0},
                    {"x": 30, "y": 30},
                ],
            },
        ],
        "ports": [
            {
                "name": "B",
                "layer": 40,
                "datatype": 0,
                "rectangle": {"x": 0, "y": 0, "width": 10, "height": 10},
                "side": "bottom",
                "direction": "input",
            }
        ],
    }
    layout._device_layer_name = lambda number, datatype: layer_names[
        (int(number), int(datatype))
    ]
    layout._device_side = lambda side, orientation: side

    layout.device("nmos", name="M1")
    assert layout.layers["Substrate"] == {"number": 40, "datatype": 0}
    assert layout.layers["GatPoly.pin"] == {"number": 5, "datatype": 2}
    assert layout.layers["HeatTrans"] == {"number": 51, "datatype": 0}


def test_layout_api_manifest_and_filters():
    runner = load_runner()
    manifest = runner.layout_api_manifest(include_examples=True)
    assert manifest["api_version"] == runner.LAYOUT_API_VERSION
    classes = {item["name"] for item in manifest["classes"]}
    for name in (
        "Layout",
        "Group",
        "Row",
        "Column",
        "Grid",
        "Stack",
        "Repeat",
        "Layer",
        "Shape",
        "DevicePlacement",
        "LayoutInstancePlacement",
        "Route",
    ):
        assert name in classes, name
    qualnames = {item["qualname"] for item in manifest["methods"]}
    for name in (
        "Layout.plan",
        "Layout.finish_placement",
        "Layout.device",
        "Layout.row",
        "Row.add",
        "Layer.rectangle",
    ):
        assert name in qualnames, name
    assert not any(item["name"].startswith("_") for item in manifest["methods"])
    device = next(
        item for item in manifest["methods"] if item["qualname"] == "Layout.device"
    )
    assert "device_name" in device["signature"], device
    assert any(
        parameter["name"] == "device_name" for parameter in device["parameters"]
    ), device

    index = runner.layout_api_lookup()
    assert index["api_version"] == runner.LAYOUT_API_VERSION
    assert any(item["name"] == "device" for item in index["methods"]), index
    assert "canonical_example" in index, index

    detail = runner.layout_api_lookup(name="device")
    assert detail["method"]["qualname"] == "Layout.device", detail
    assert detail["method"]["example"], detail

    search = runner.layout_api_lookup(query="route")
    assert any(item["name"] == "route" for item in search["methods"]), search

    class_view = runner.layout_api_lookup(class_name="Layout", limit=5)
    assert class_view["total"] > 5, class_view
    assert len(class_view["methods"]) == 5, class_view

    with tempfile.TemporaryDirectory(prefix="ice-layout-api-") as directory:
        pdk_root = Path(directory) / "fake-pdk"
        devices = pdk_root / "layout" / "devices"
        devices.mkdir(parents=True)
        (devices / "index.json").write_text(
            json.dumps(
                {
                    "cells": [
                        {
                            "name": "nfet",
                            "parameters": [
                                {"name": "model", "value": "nfet_01v8"},
                                {"name": "w", "value": "1u"},
                                {"name": "l", "value": "0.15u"},
                            ],
                        },
                        {
                            "name": "pfet",
                            "parameters": [
                                {"name": "model", "value": "pfet_01v8"},
                                {"name": "w", "value": "1u"},
                                {"name": "l", "value": "0.15u"},
                            ],
                        },
                    ]
                }
            )
        )
        pdk_manifest = runner.layout_api_manifest(
            pdk="sky130A", pdk_root=pdk_root, include_examples=True
        )
        assert pdk_manifest["devices"]["names"] == ["nfet", "pfet"], pdk_manifest
        assert 'layout.device("nfet"' in pdk_manifest["canonical_example"], pdk_manifest
        device_detail = runner.layout_api_lookup(
            name="nfet", pdk="sky130A", pdk_root=pdk_root
        )
        assert device_detail["device"]["name"] == "nfet", device_detail
        assert any(
            parameter["name"] == "model"
            for parameter in device_detail["device"]["parameters"]
        ), device_detail
        device_search = runner.layout_api_lookup(
            query="nfet", pdk="sky130A", pdk_root=pdk_root
        )
        assert device_search["devices"]["entries"][0]["name"] == "nfet", device_search


def test_layout_validate_script():
    runner = load_runner()
    canonical = runner.layout_api_manifest()["canonical_example"]
    result = runner.validate_layout_script(canonical)
    assert result["ok"], result

    bad_add = (
        'layout.plan(devices=["X1"], rows=1)\n'
        'X1 = layout.add("X1", "nmos", w=20, l=0.13)\n'
        "layout.finish_placement()\n"
    )
    result = runner.validate_layout_script(bad_add)
    assert not result["ok"], result
    diagnostics = {item["code"]: item for item in result["diagnostics"]}
    assert "layout.api.device_constructor_misuse" in diagnostics, diagnostics
    assert (
        "layout.device"
        in diagnostics["layout.api.device_constructor_misuse"]["suggestion"]
    )

    missing_device = (
        'layout.plan(devices=["M1", "M2"], rows=1)\n'
        'M1 = layout.device("nmos", name="M1")\n'
        "layout.add(M1)\n"
        "layout.finish_placement()\n"
    )
    result = runner.validate_layout_script(missing_device)
    codes = {item["code"] for item in result["diagnostics"]}
    assert "layout.contract.missing_device" in codes, result

    unadded_device = (
        'layout.plan(devices=["M1", "M2"], rows=1)\n'
        'M1 = layout.device("nmos", name="M1")\n'
        'M2 = layout.device("nmos", name="M2")\n'
        "layout.add(M2)\n"
        "layout.finish_placement()\n"
    )
    result = runner.validate_layout_script(unadded_device)
    codes = {item["code"] for item in result["diagnostics"]}
    assert "layout.contract.unadded_device" in codes, result

    grouped = (
        'layout.plan(devices=["M1", "M2"], rows=1)\n'
        'M1 = layout.device("nmos", name="M1")\n'
        'M2 = layout.device("nmos", name="M2")\n'
        "row = layout.row([M1, M2], gap=0)\n"
        "layout.add(row)\n"
        "layout.finish_placement()\n"
    )
    assert runner.validate_layout_script(grouped)["ok"]

    missing_finish = (
        'layout.plan(devices=["M1"], rows=1)\n'
        'M1 = layout.device("nmos", name="M1")\n'
        "layout.add(M1)\n"
    )
    result = runner.validate_layout_script(missing_finish)
    codes = {item["code"] for item in result["diagnostics"]}
    assert "layout.contract.missing_finish" in codes, result

    unknown_method = (
        "layout.plan(devices=[])\nlayout.rwo()\nlayout.finish_placement()\n"
    )
    result = runner.validate_layout_script(unknown_method)
    diagnostics = {item["code"]: item for item in result["diagnostics"]}
    assert "layout.api.unknown_method" in diagnostics, diagnostics
    assert "row" in diagnostics["layout.api.unknown_method"]["suggestion"]

    syntax_error = "layout.plan(devices=[]\n"
    result = runner.validate_layout_script(syntax_error)
    assert not result["ok"], result
    assert result["diagnostics"][0]["code"] == "layout.syntax", result


def test_output_metadata_and_determinism():
    runner = load_runner()

    def build():
        layout = runner.Layout()
        layout.parameter("width", "10")
        first = rectangle(layout, 0, 0, 10, 10, "a")
        second = rectangle(layout, 0, 0, 10, 10, "b")
        row = layout.row(gap=0)
        row.add(first)
        row.add(second)
        layout.align(first, second, line="h_center")
        layout.add(row)
        return layout.to_dict()

    first = build()
    second = build()
    assert first == second
    assert first["builder_version"] >= 2
    assert first["format"] == "ice-procedural-layout"
    assert "constraints" in first


def main():
    tests = [
        test_group_row_and_column,
        test_nested_composition_and_measurement,
        test_mirror_anchor_and_pin,
        test_grid_stack_and_repeat,
        test_constraints,
        test_inline_and_builtin_generators,
        test_package_generator_registry,
        test_generator_discovery_command,
        test_layout_plan_gate_and_review,
        test_layout_plan_gate_rejects_low_utilization,
        test_report_groups_rows_before_measuring_gaps,
        test_layout_instance_composition,
        test_layout_instance_connect,
        test_device_composition,
        test_device_layers_keep_real_stream_numbers,
        test_layout_api_manifest_and_filters,
        test_layout_validate_script,
        test_output_metadata_and_determinism,
    ]
    for test in tests:
        test()
        print(f"[pass] {test.__name__}")
    print("procedural composition tests passed")


if __name__ == "__main__":
    main()
