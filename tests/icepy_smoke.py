"""Small smoke test for the optional icepy bindings.

Run from the repository root with the build directory and python/ on PYTHONPATH:

    PYTHONPATH=build/linux/x86_64/release:python python3 tests/icepy_smoke.py

If ICE_IHP_ROOT points to an IHP checkout, the IHP nmos PCell is generated
through icepy.pcells as well.
"""

from __future__ import annotations

import os
from pathlib import Path
import tempfile

import icepy


def test_layout_builder() -> None:
    project = icepy.new_project("icepy-smoke")
    builder = project.create_layout_cell("top", "generic")
    builder.layer("Metal1", 8)
    builder.rectangle("Metal1", 0, 0, 4000, 600)
    builder.polygon(
        "Metal2",
        [
            icepy.LayoutPoint(0, 1000),
            icepy.LayoutPoint(4000, 1000),
            icepy.LayoutPoint(2000, 3000),
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
    assert cell.ports[0].direction == icepy.PortDirection.input
    assert cell.ports[0].side == icepy.PortSide.left
    assert cell.ports[0].label == "VIN"
    assert len(cell.nets) == 1
    assert cell.ports[0].net == cell.nets[0].id
    assert cell.parameters[0].value == "1u"
    assert cell.bounds is not None
    assert project.validate() == 0

    serialized = project.serialize()
    round_trip = icepy.Project.deserialize(serialized)
    assert round_trip.serialize() == serialized
    assert len(round_trip.layout_cells) == 1

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory) / "split-project"
        project.save(str(root))
        assert (root / "project.icepro").is_file()
        assert (root / "lay" / "top.icelay").is_file()
        assert (root / "scripts").is_dir()
        assert (root / "build").is_dir()
        loaded = icepy.load_project(str(root / "project.icepro"))
        assert loaded.name == project.name
        assert len(loaded.layout_cells) == 1


def test_procedural_layout() -> None:
    project = icepy.new_project("icepy-procedural-smoke")
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
    root = os.environ.get("ICE_IHP_ROOT")
    if not root:
        candidate = Path.home() / "repositories" / "IHP"
        if candidate.is_dir():
            root = str(candidate)
    if not root:
        print(
            "IHP PCell test skipped: ICE_IHP_ROOT is not set and no checkout was found"
        )
        return
    project = icepy.new_project("icepy-pcell-smoke")
    cell = icepy.pcells.generate_cell(
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
    print(f"IHP nmos generated {len(cell.shapes)} shapes through icepy")


if __name__ == "__main__":
    test_layout_builder()
    test_procedural_layout()
    test_ihp_pcell_runtime()
    print("icepy smoke tests passed")
