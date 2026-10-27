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
    builder.port("P1", "Metal1", 0, 0, 200, 200)
    builder.parameter("w", "1u")
    cell = builder.cell()
    assert cell.name == "top"
    assert len(cell.shapes) == 2
    assert len(cell.ports) == 1
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
        assert (root / "reports").is_dir()
        assert (root / "build").is_dir()
        loaded = icepy.load_project(str(root / "project.icepro"))
        assert loaded.name == project.name
        assert len(loaded.layout_cells) == 1


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
    test_ihp_pcell_runtime()
    print("icepy smoke tests passed")
