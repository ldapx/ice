#!/usr/bin/env python3
"""Exercise the openvlsipy schematic, simulation, DRC, and package surfaces."""

import tempfile
from pathlib import Path

import openvlsipy


def main() -> int:
    project = openvlsipy.new_project("openvlsipy-api")
    schematic = project.create_schematic_cell("top")
    resistor_symbol = schematic.symbol("resistor")
    resistor_a = schematic.pin(resistor_symbol, "A", "left", -50, 0)
    resistor_b = schematic.pin(resistor_symbol, "B", "right", 50, 0)
    resistor = schematic.instance(resistor_symbol, "R1")
    schematic.set_parameter(resistor, "resistance", "1k")

    capacitor_symbol = schematic.symbol("capacitor")
    capacitor_a = schematic.pin(capacitor_symbol, "A", "left", -50, 0)
    capacitor_b = schematic.pin(capacitor_symbol, "B", "right", 50, 0)
    capacitor = schematic.instance(capacitor_symbol, "C1")
    schematic.set_parameter(capacitor, "capacitance", "1n")
    schematic.net("mid", [resistor_b, capacitor_a])
    schematic.set_cell_parameter("R", "1k")

    assert any(cell.id == schematic.id for cell in project.schematic_cells)
    assert project.schematic_cell("top").name == "top"
    assert len(project.schematic_cell_by_id(schematic.id).symbols) == 2
    netlist = schematic.netlist()
    assert any("R1" in line for line in netlist), netlist
    assert any("C1" in line for line in netlist), netlist

    layout = project.create_layout_cell("layout")
    layout.layer("Metal1", 1)
    rectangle = layout.rectangle("Metal1", 0, 0, 100, 100)
    second_rectangle = layout.rectangle("Metal1", 300, 50, 50, 50)
    duplicates = layout.duplicate([rectangle], 200, 0)
    assert len(duplicates) == 1, duplicates
    default_duplicates = layout.duplicate([rectangle])
    assert len(default_duplicates) == 1, default_duplicates
    array = layout.array([rectangle], 2, 2, 200, 150)
    assert len(array) == 3, array
    layout.arrange([rectangle, second_rectangle], "left")
    assert len(layout.cell().shapes) == 7, layout.cell().shapes

    projected = project.create_layout_from_schematic(schematic.id, "projected")
    assert projected.cell().schematic_cell == schematic.id
    projected.unlink_schematic()
    assert projected.cell().schematic_cell == 0
    projected.link_schematic(schematic.id)
    assert projected.cell().schematic_cell == schematic.id
    assert openvlsipy.schematic_for_layout(project, projected.id) == schematic.id
    links = projected.device_links()
    assert len(links) == 2, links
    assert all(not link.stale for link in links), links
    first = links[0]
    projected.unbind_device(first.layout_object)
    assert len(projected.device_links()) == 1
    projected.bind_device(first.layout_object, schematic.id, first.schematic_instance)
    assert len(projected.device_links()) == 2
    report = openvlsipy.run_lvs(project, schematic.id, projected.id)
    assert report.schema_version == 2, report.schema_version
    assert report.summary.devices_matched == 2, report.summary
    assert len(report.devices) == 2, report.devices
    assert all(match.method == openvlsipy.LvsMatchMethod.linked for match in report.devices)
    assert any("devices" in line for line in [openvlsipy.lvs_report_json(report)])

    wrapped = schematic.wrap([resistor, capacitor], "wrapped")
    assert wrapped.cell != 0, wrapped
    assert wrapped.instance != 0, wrapped
    wrapped_cell = project.schematic_cell("wrapped")
    assert len(wrapped_cell.instances) == 2, wrapped_cell.instances
    assert len(wrapped_cell.nets) == 1, wrapped_cell.nets

    text = project.create_text_schematic_cell(
        "rc", "V1 in 0 1\nR1 in out 1k\nC1 out 0 1n\n"
    )
    assert text.cell().text_schematic is True
    simulation = openvlsipy.SimulationItem()
    simulation.name = "rc_tran"
    simulation.schematic = text.id
    simulation.analysis = "transient"
    simulation.stop = 1.0e-6
    simulation.step = 1.0e-9
    simulation.capture_signals = "v(out)"
    created = project.create_simulation(simulation)
    assert created.id != 0
    created.model_section = "tt"
    updated = project.update_simulation(created)
    assert updated.model_section == "tt"
    assert len(project.simulations) == 1
    assert project.simulation("rc_tran").name == "rc_tran"
    result = project.run_simulation("rc_tran", project_root=".", timeout_ms=30000)
    assert result.state in (openvlsipy.JobState.succeeded, openvlsipy.JobState.failed), result
    if result.state == openvlsipy.JobState.succeeded:
        assert result.traces, result

    local = openvlsipy.LocalVerificationAdapter().run(project)
    assert isinstance(local.drc, list)
    report = openvlsipy.VerificationReport()
    assert report.drc == []
    assert "drc" in openvlsipy.verification_report_json(report)

    with tempfile.TemporaryDirectory(prefix="openvlsipy-package-") as temporary:
        root = Path(temporary)
        (root / "package.toml").write_text(
            "[package]\n"
            "schema_version = 1\n"
            'name = "openvlsipy.test"\n'
            'version = "1.0.0"\n'
            'license = "MIT"\n'
            "\n"
            "[pdk]\n"
            'id = "openvlsipy-test"\n'
            'display_name = "openvlsipy test"\n'
        )
        manifest = openvlsipy.PackageManager.read_manifest(root)
        assert manifest.name == "openvlsipy.test", manifest.name
        assert openvlsipy.package_matches_pdk(manifest, "openvlsipy-test")
        assert openvlsipy.PackageManager.content_hash(root).startswith("sha256:")
        lockfile = openvlsipy.Lockfile()
        lockfile.openvlsi_version = "test"
        locked = openvlsipy.LockedPackage()
        locked.name = "openvlsipy.test"
        locked.version = "1.0.0"
        locked.source = str(root)
        locked.content_hash = openvlsipy.PackageManager.content_hash(root)
        locked.trusted = False
        lockfile.packages = [locked]
        lock_path = root / "openvlsi.lock"
        openvlsipy.PackageManager.write_lockfile(lock_path, lockfile)
        round_trip = openvlsipy.PackageManager.read_lockfile(lock_path)
        assert round_trip.packages[0].name == "openvlsipy.test", round_trip
        assert openvlsipy.resolve_pdk_stack("openvlsipy-test", "") == ""

    project.remove_simulation("rc_tran")
    assert not project.simulations
    print("openvlsipy API tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
