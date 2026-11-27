"""Small smoke test for the optional openvlsipy bindings.

Run from the repository root with the build directory and python/ on PYTHONPATH:

    PYTHONPATH=build/linux/x86_64/release:python python3 tests/openvlsipy_smoke.py

If the IHP package is available, its package-local PCell runtime generates the
IHP nmos PCell through openvlsipy as well.
"""

from __future__ import annotations

import os
from pathlib import Path
import sys
import tempfile

import openvlsipy


def test_layout_builder() -> None:
    project = openvlsipy.new_project("openvlsipy-smoke")
    builder = project.create_layout_cell("top", "generic")
    builder.layer("Metal1", 8)
    builder.rectangle("Metal1", 0, 0, 4000, 600)
    builder.polygon(
        "Metal2",
        [
            openvlsipy.LayoutPoint(0, 1000),
            openvlsipy.LayoutPoint(4000, 1000),
            openvlsipy.LayoutPoint(2000, 3000),
        ],
    )
    builder.layout_net("VIN")
    builder.port(
        "P1",
        "Metal1",
        0,
        0,
        200,
        200,
        direction="input",
        side="left",
        net="VIN",
        label="VIN",
    )
    builder.parameter("w", "1u")
    cell = builder.cell()
    assert cell.name == "top"
    assert len(cell.shapes) == 2
    assert len(cell.ports) == 1
    assert cell.ports[0].direction == openvlsipy.PortDirection.input
    assert cell.ports[0].side == openvlsipy.PortSide.left
    assert cell.ports[0].label == "VIN"
    assert len(cell.nets) == 1
    assert cell.ports[0].net == cell.nets[0].id
    assert cell.parameters[0].value == "1u"
    assert cell.bounds is not None
    assert project.validate() == 0

    serialized = project.serialize()
    round_trip = openvlsipy.Project.deserialize(serialized)
    assert round_trip.serialize() == serialized
    assert len(round_trip.layout_cells) == 1

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory) / "split-project"
        project.save(str(root))
        assert (root / "project.ovpro").is_file()
        assert (root / "lay" / "top.ovlay").is_file()
        assert (root / "scripts").is_dir()
        assert (root / "build").is_dir()
        loaded = openvlsipy.load_project(str(root / "project.ovpro"))
        assert loaded.name == project.name
        assert len(loaded.layout_cells) == 1


def test_procedural_layout() -> None:
    project = openvlsipy.new_project("openvlsipy-procedural-smoke")
    procedural = project.create_procedural_layout(
        "generated_top",
        "generic",
        "",
        'width = layout.um(layout.parameter("width", "4"))\n'
        'metal = layout.layer("Metal1", 1)\n'
        'shape = metal.rectangle(layout.um(0), layout.um(0), width, layout.um(1))\n'
        'shape.name = "body"\n'
        'shape.net = "VDD"\n'
        'metal.path([(0, 0), (4000, 0)], layout.um(0.5))\n'
        'layout.port("VDD", "Metal1", 0, 0, 1000, 1000, net="VDD")\n',
        {"width": "4"},
    )
    assert procedural.needs_regeneration
    assert len(project.procedural_layouts) == 1
    generated = project.regenerate_procedural_layout("generated_top")
    assert not generated.needs_regeneration
    assert generated.parameters and generated.parameters[0].name == "width"
    changed = project.set_procedural_parameter("generated_top", "width", "6")
    assert changed.needs_regeneration
    generated = project.regenerate_procedural_layout("generated_top")
    assert not generated.needs_regeneration
    assert generated.generated_script_hash
    cell = project.layout_cell_by_id(generated.generated_cell)
    assert cell.read_only
    assert len(cell.shapes) == 2
    assert len(cell.ports) == 1
    assert len(cell.nets) == 1
    assert project.validate() == 0


def test_ihp_pcell_runtime() -> None:
    package = os.environ.get("OPENVLSI_IHP_PACKAGE")
    if not package:
        candidates = [
            Path.home() / "repositories" / "openvlsi-pkg-ihp-sg13g2",
            Path.home() / ".openvlsi" / "packages" / "ihp.sg13g2",
        ]
        for candidate in candidates:
            if candidate.is_dir():
                package = str(candidate)
                break
    if not package:
        print("IHP PCell test skipped: the IHP package was not found")
        return
    package_root = Path(package)
    sys.path.insert(0, str(package_root / "layout" / "devices" / "pcell"))
    from ihp_runtime import generate_cell

    root = os.environ.get("OPENVLSI_IHP_ROOT")
    if not root:
        vendored = package_root / "vendor" / "ihp"
        if vendored.is_dir():
            root = str(vendored)
    if not root:
        print("IHP PCell test skipped: no IHP source tree was found")
        return
    project = openvlsipy.new_project("openvlsipy-pcell-smoke")
    cell = generate_cell(
        project,
        "IHP nmos_smoke",
        root,
        "nmos_code",
        "nmos",
        {"w": "1u", "l": "0.13u", "ng": "1"},
    )
    assert cell.shapes
    assert cell.bounds is not None
    assert project.validate() == 0
    print(f"IHP nmos generated {len(cell.shapes)} shapes through the package runtime")


if __name__ == "__main__":
    test_layout_builder()
    test_procedural_layout()
    test_ihp_pcell_runtime()
    print("openvlsipy smoke tests passed")
