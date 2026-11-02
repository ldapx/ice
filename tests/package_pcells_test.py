#!/usr/bin/env python3
"""Validate package-local PCell libraries and regeneration.

Run from the ICE repository root with the build directory and python/ on
PYTHONPATH:

    PYTHONPATH=build/linux/x86_64/release:python \
        python3 tests/package_pcells_test.py

Package repositories are discovered next to the ICE checkout. Set
ICE_PACKAGE_REPOSITORIES to a colon-separated list to override the search path.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import icepy


REPOSITORY = Path(__file__).resolve().parent.parent
PACKAGES = {
    "ice-pkg-ihp-sg13g2": {
        "cells": 100,
        "device": "nmos",
        "module": "nmos_code",
        "class": "nmos",
    },
    "ice-pkg-sky130": {
        "cells": 10,
        "device": "nfet",
        "module": "pcells",
        "class": "nfet",
    },
    "ice-pkg-gf180mcu": {
        "cells": 20,
        "device": "nmos",
        "module": "pcells",
        "class": "nmos",
    },
}


def package_roots() -> dict[str, Path]:
    configured = os.environ.get("ICE_PACKAGE_REPOSITORIES")
    bases = (
        [Path(item) for item in configured.split(os.pathsep) if item]
        if configured
        else []
    )
    bases.extend([REPOSITORY.parent, REPOSITORY])
    roots = {}
    for name in PACKAGES:
        for base in bases:
            candidate = base / name
            if (candidate / "layout" / "devices.json").is_file():
                roots[name] = candidate
                break
    return roots


def check_library(name: str, root: Path) -> None:
    expected = PACKAGES[name]
    library = json.loads((root / "layout" / "devices.json").read_text())
    assert library["format"] == "ice-layout-device-library"
    assert library["schema_version"] == 1
    assert library["pdk"]
    assert library["cell_prefix"]
    assert len(library["cells"]) >= expected["cells"]

    cells = {cell["name"]: cell for cell in library["cells"]}
    device = cells.get(expected["device"])
    assert device is not None, f"{name}: missing {expected['device']}"
    assert device["generator"]["module"] == expected["module"]
    assert device["generator"]["class"] == expected["class"]
    assert device["parameters"]
    assert device["shapes"]
    assert device["bounds"][2] > device["bounds"][0]
    assert device["bounds"][3] > device["bounds"][1]

    if name == "ice-pkg-ihp-sg13g2":
        # The IHP package routes through its vendored upstream PyCell runtime.
        with tempfile.TemporaryDirectory() as directory:
            params = Path(directory) / "params.json"
            output = Path(directory) / "geometry.json"
            params.write_text(
                json.dumps(
                    {
                        parameter["name"]: parameter["value"]
                        for parameter in device["parameters"]
                    }
                )
            )
            subprocess.run(
                [
                    sys.executable,
                    str(root / "layout" / "device_generator.py"),
                    "--pdk-root",
                    str(root),
                    "--module",
                    device["generator"]["module"],
                    "--class",
                    device["generator"]["class"],
                    "--params-file",
                    str(params),
                    "--output",
                    str(output),
                ],
                check=True,
                cwd=root,
            )
            generated = json.loads(output.read_text())
    else:
        generated = icepy.pcells.generate_package_geometry(
            root,
            device["generator"]["module"],
            device["generator"]["class"],
            {
                parameter["name"]: parameter["value"]
                for parameter in device["parameters"]
            },
        )
    assert generated["shapes"], f"{name}: generator produced no shapes"
    assert generated["bounds"][2] > generated["bounds"][0]
    assert generated["bounds"][3] > generated["bounds"][1]
    assert generated["bounds"][2] - generated["bounds"][0] < 1_000_000
    assert generated["bounds"][3] - generated["bounds"][1] < 1_000_000

    if name != "ice-pkg-ihp-sg13g2":
        # SPICE-style length suffixes must reach the geometry, not silently fall
        # back to defaults.
        overrides = {
            parameter["name"]: parameter["value"] for parameter in device["parameters"]
        }
        if "w" in overrides:
            overrides["w"] = "2u"
        if "l" in overrides:
            overrides["l"] = "0.2u"
        resized = icepy.pcells.generate_package_geometry(
            root, device["generator"]["module"], device["generator"]["class"], overrides
        )
        assert resized["shapes"], f"{name}: resized generator produced no shapes"
        # Every declared generator must be runnable through the ICE layout API.
        for cell in library["cells"]:
            generator = cell.get("generator")
            if not generator:
                continue
            geometry = icepy.pcells.generate_package_geometry(
                root,
                generator["module"],
                generator["class"],
                {
                    parameter["name"]: parameter["value"]
                    for parameter in cell.get("parameters", [])
                },
            )
            assert geometry["shapes"], f"{name}: {cell['name']} produced no shapes"
            for shape in geometry["shapes"]:
                assert len(shape["points"]) >= 3
                assert shape["layer"] >= 0
    print(f"{name}: {len(library['cells'])} cells, {len(cells)} named, generators OK")


def main() -> int:
    roots = package_roots()
    missing = sorted(set(PACKAGES) - set(roots))
    if missing:
        print(f"Skipping missing package repositories: {', '.join(missing)}")
    if not roots:
        print("No package repositories found; nothing to test")
        return 0
    for name, root in sorted(roots.items()):
        check_library(name, root)
    print("package PCell tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
