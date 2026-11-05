#!/usr/bin/env python3
"""Smoke-test the ice-mcp schematic, simulation, and diagnostics tools.

Run after building ice-mcp and ice-cli:
    python3 tests/mcp_tools_smoke.py
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ICE_MCP = ROOT / "build/linux/x86_64/release/ice-mcp"
ICE_CLI = ROOT / "build/linux/x86_64/release/ice-cli"


class MCP:
    def __init__(self, project: Path | None = None, cwd: Path | None = None):
        command = [str(ICE_MCP)]
        if project is not None:
            command += ["--project", str(project)]
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            cwd=cwd,
        )
        self.next_id = 1
        self.call(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "mcp-tools-smoke", "version": "1"},
            },
        )

    def call(self, method: str, params: dict) -> dict:
        self.next_id += 1
        request = {
            "jsonrpc": "2.0",
            "id": self.next_id,
            "method": method,
            "params": params,
        }
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError(
                    "ice-mcp closed unexpectedly:\n" + self.process.stderr.read()
                )
            try:
                message = json.loads(line)
            except json.JSONDecodeError as error:
                raise RuntimeError(
                    f"Invalid JSON-RPC response for {method}: {error}: {line[:500]!r}"
                ) from error
            if message.get("id") != self.next_id:
                continue
            if "error" in message:
                raise RuntimeError(json.dumps(message["error"]))
            return message["result"]

    def tool(self, tool_name: str, **arguments) -> dict:
        result = self.call("tools/call", {"name": tool_name, "arguments": arguments})
        text = result["content"][0]["text"]
        if result.get("isError"):
            raise RuntimeError(text)
        try:
            return json.loads(text)
        except json.JSONDecodeError as error:
            raise RuntimeError(
                f"Invalid JSON from {tool_name}: {error}: {text[:500]!r}"
            ) from error

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()


def main() -> int:
    if not ICE_MCP.is_file() or not ICE_CLI.is_file():
        raise SystemExit("Build ice-mcp and ice-cli before running this smoke test.")

    cli_version = json.loads(subprocess.check_output([str(ICE_CLI), "--version"]))
    assert cli_version.get("commit"), cli_version
    assert cli_version.get("build_time"), cli_version
    assert "project_repair" in cli_version.get("features", []), cli_version

    with tempfile.TemporaryDirectory(prefix="ice-mcp-smoke-") as temporary:
        project = Path(temporary) / "project"
        subprocess.run(
            [str(ICE_CLI), "project-create", str(project), "MCP Smoke"],
            check=True,
            capture_output=True,
        )

        mcp = MCP(project)
        try:
            tools = {tool["name"] for tool in mcp.call("tools/list", {})["tools"]}
            for required in {
                "ice_doctor",
                "schematic_list",
                "schematic_info",
                "schematic_add_symbol",
                "schematic_add_pin",
                "schematic_add_net",
                "schematic_netlist",
                "project_repair",
                "simulation_result",
                "simulation_signals",
                "simulation_measure",
                "pdk_info",
                "pdk_build",
                "project_migrate",
                "schematic_add_text",
                "schematic_create_systemverilog",
                "schematic_hdl_ports",
                "schematic_add_hdl_instance",
                "schematic_set_parameter",
                "schematic_validate_hierarchy",
                "schematic_add_component",
                "schematic_list_symbols",
                "schematic_add_circuit",
                "schematic_validate",
                "schematic_import_spice",
                "simulation_trace",
                "ice_help",
                "layout_add_instance",
                "layout_add_port",
                "layout_list_ports",
                "layout_update_port",
                "layout_remove_port",
                "layout_add_net",
                "layout_assign_port_net",
                "layout_info",
                "layout_connectivity",
                "layout_export_gds",
                "layout_export_lef",
                "layout_export_def",
                "pex_run",
            }:
                if required not in tools:
                    raise RuntimeError(f"Missing MCP tool: {required}")

            version = mcp.tool("ice_version")
            assert version.get("commit"), version
            assert version.get("build_time"), version
            assert "project_repair" in version.get("features", []), version
            assert "pdk_build" in version.get("features", []), version
            assert "layout_pex" in version.get("features", []), version
            assert "pex_device_extraction" in version.get("features", []), version
            assert "polygon_device_extraction" in version.get("features", []), version
            assert "systemverilog_cells" in version.get("features", []), version
            assert "digital_hdl" in version.get("features", []), version
            assert "vcd_graphs" in version.get("features", []), version
            assert "hdl_hierarchy" in version.get("features", []), version
            assert "mixed_signal" in version.get("features", []), version
            assert "agent_authoring" in version.get("features", []), version
            assert "simulation_trace" in version.get("features", []), version

            mcp.tool("schematic_create", name="ring_test", pdk="sky130")
            for symbol in ("inv1", "inv2"):
                mcp.tool(
                    "schematic_add_symbol",
                    cell="ring_test",
                    name=symbol,
                    pins=[
                        {"name": "A", "x": 0, "y": 0, "direction": "left"},
                        {"name": "Y", "x": 100, "y": 0, "direction": "right"},
                    ],
                )
            mcp.tool(
                "schematic_add_instance",
                cell="ring_test",
                symbol="inv1",
                name="I1",
                x=0,
                y=0,
            )
            mcp.tool(
                "schematic_add_instance",
                cell="ring_test",
                symbol="inv2",
                name="I2",
                x=200,
                y=0,
            )
            mcp.tool(
                "schematic_add_net", cell="ring_test", name="n1", pins=["I1.Y", "I2.A"]
            )

            info = mcp.tool("schematic_info", schematic="ring_test")
            assert len(info["symbols"]) == 2
            assert len(info["instances"]) == 2
            assert len(info["nets"]) == 1
            assert info["symbols"][0]["pins"]
            mcp.tool("schematic_netlist", schematic="ring_test")

            mcp.tool("layout_create", name="pin_test")
            mcp.tool("layout_add_layer", name="Metal1", number=8)
            mcp.tool("layout_add_net", cell="pin_test", name="VIN")
            mcp.tool(
                "layout_add_port",
                cell="pin_test",
                name="VIN",
                layer="Metal1",
                x=0,
                y=0,
                width=400,
                height=400,
                direction="input",
                side="left",
                net="VIN",
                label="VIN",
            )
            ports = mcp.tool("layout_list_ports", cell="pin_test")["ports"]
            assert len(ports) == 1, ports
            assert ports[0]["direction"] == "input", ports
            info = mcp.tool("layout_info", cell="pin_test")
            assert info["ports"][0]["name"] == "VIN", info
            assert info["nets"][0]["name"] == "VIN", info
            mcp.tool(
                "layout_add_rectangle",
                cell="pin_test",
                layer="Metal1",
                x=0,
                y=0,
                width=2000,
                height=2000,
            )
            connectivity = mcp.tool("layout_connectivity", cell="pin_test")
            assert connectivity["nets"], connectivity
            pex = mcp.tool("pex_run", layout="pin_test")
            assert pex["schematic"], pex
            assert pex["nets"], pex
            assert "devices" in pex and not pex["devices"], pex
            assert pex["devices_from_layout"] is False, pex
            assert "RPEX1 VIN PEXRC1" in pex["source"], pex
            assert "CPEX1 PEXRC1 0" in pex["source"], pex
            exports = Path(temporary) / "exports"
            exports.mkdir()
            for tool_name, suffix, marker in (
                ("layout_export_gds", ".gds", None),
                ("layout_export_lef", ".lef", "PIN VIN"),
                ("layout_export_def", ".def", "VIN"),
            ):
                path = exports / ("pin_test" + suffix)
                mcp.tool(tool_name, cell="pin_test", path=str(path))
                assert path.is_file() and path.stat().st_size > 0, path
                if marker:
                    assert marker in path.read_text(), (tool_name, path.read_text())
            mcp.tool(
                "layout_update_port",
                cell="pin_test",
                port="VIN",
                direction="power",
                side="right",
                visible=False,
            )
            updated = mcp.tool("layout_list_ports", cell="pin_test")["ports"][0]
            assert updated["direction"] == "power", updated
            assert updated["visible"] is False, updated
            mcp.tool("layout_assign_port_net", cell="pin_test", port="VIN", net=None)
            cleared = mcp.tool("layout_list_ports", cell="pin_test")["ports"][0]
            assert cleared["net"] is None, cleared
            mcp.tool("layout_remove_port", cell="pin_test", port="VIN")
            assert not mcp.tool("layout_list_ports", cell="pin_test")["ports"]

            # Regression: the advertised 'schematic' key must work without
            # also supplying the legacy 'cell' key.
            text_cell = mcp.tool(
                "schematic_create_text",
                name="text_test",
                text="* rc\nV1 in 0 1\nR1 in out 1k\nC1 out 0 1p\n.tran 1n 1u\n.end\n",
            )
            mcp.tool(
                "schematic_set_text",
                schematic=text_cell["id"],
                text="* rc updated\nV1 in 0 1\nR1 in out 1k\nC1 out 0 1p\n.tran 1n 1u\n.end\n",
            )
            kind_text = mcp.tool(
                "schematic_create",
                name="kind_text",
                kind="text",
                text="* kind text\nV1 in 0 1\nR1 in out 1k\nC1 out 0 1p\n.end\n",
            )
            assert kind_text["kind"] == "text", kind_text
            alias_text = mcp.tool(
                "schematic_add_text",
                name="alias_text",
                text="* alias text\nV1 in 0 1\nR1 in out 1k\nC1 out 0 1p\n.end\n",
            )
            assert alias_text["kind"] == "text", alias_text
            mcp.tool("schematic_create", name="convert_me")
            converted = mcp.tool(
                "schematic_set_text",
                schematic="convert_me",
                text="* converted\nV1 in 0 1\nR1 in out 1k\nC1 out 0 1p\n.end\n",
                convert=True,
            )
            assert converted["converted"] is True, converted

            sv_source = (
                "`timescale 1ns/1ps\n"
                "module sv_smoke_tb;\n"
                "    logic clk = 1'b0;\n"
                "    logic q = 1'b0;\n"
                "    always #5 clk = ~clk;\n"
                "    always_ff @(posedge clk) q <= ~q;\n"
                "    initial #40 $finish;\n"
                "endmodule\n"
            )
            sv_cell = mcp.tool(
                "schematic_create_systemverilog",
                name="sv_smoke",
                text=sv_source,
            )
            assert sv_cell["kind"] == "systemverilog", sv_cell
            sv_info = mcp.tool("schematic_info", schematic="sv_smoke")
            assert sv_info["systemverilog"] is True, sv_info
            assert "module sv_smoke_tb" in sv_info["source"], sv_info
            updated_sv = mcp.tool(
                "schematic_set_text",
                schematic="sv_smoke",
                kind="systemverilog",
                text=sv_source + "// updated through schematic_set_text\n",
            )
            assert updated_sv["kind"] == "systemverilog", updated_sv
            sv_kind = mcp.tool(
                "schematic_create",
                name="sv_kind",
                kind="systemverilog",
                text="module sv_kind(input logic clk);\nendmodule\n",
            )
            assert sv_kind["kind"] == "systemverilog", sv_kind

            mcp.tool(
                "simulation_create",
                name="sv_sim",
                schematic="sv_smoke",
            )
            sv_run = mcp.tool("simulation_run", simulation="sv_sim")
            assert sv_run["state"] == "succeeded", sv_run
            assert sv_run["trace_count"] >= 2, sv_run
            sv_signals = set(sv_run["available_signals"])
            assert any(name == "clk" or name.endswith(".clk") for name in sv_signals), (
                sv_run
            )
            assert any(name == "q" or name.endswith(".q") for name in sv_signals), (
                sv_run
            )
            sv_graphs = mcp.tool("simulation_graph_list", simulation="sv_sim")
            assert sv_graphs and sv_graphs[0]["y_signals"], sv_graphs
            assert any(name.endswith(".clk") for name in sv_graphs[0]["y_signals"]), (
                sv_graphs
            )
            sv_stored = mcp.tool("simulation_result", simulation="sv_sim")
            assert sv_stored["state"] == "succeeded", sv_stored
            assert sv_stored["time_points"] > 1, sv_stored
            help_result = mcp.tool("ice_help", topic="mixed-signal")
            assert "help" in help_result, help_result
            symbols = mcp.tool("schematic_list_symbols", query="resistor")
            assert any(symbol["name"] == "Resistor" for symbol in symbols["symbols"]), symbols

            mcp.tool("schematic_create", name="alias_symbols")
            alias_symbol = mcp.tool("schematic_add_symbol", cell="alias_symbols", name="RES")
            assert alias_symbol["artwork"] is True, alias_symbol
            assert {pin["name"] for pin in alias_symbol["pins"]} == {"1", "2"}, alias_symbol
            mcp.tool(
                "schematic_add_instance",
                cell="alias_symbols",
                symbol="RES",
                name="R1",
                x=0,
                y=0,
            )
            alias_validation = mcp.tool("schematic_validate", schematic="alias_symbols")
            assert alias_validation["ok"] is True, alias_validation

            mcp.tool("schematic_create", name="agent_rc")
            circuit = mcp.tool(
                "schematic_add_circuit",
                cell="agent_rc",
                components=[
                    {"type": "resistor", "name": "R1", "parameters": {"resistance": "10k"}},
                    {"type": "capacitor", "name": "C1", "parameters": {"capacitance": "1n"}},
                    {
                        "type": "voltage source",
                        "name": "V1",
                        "parameters": {"value": "PULSE(0 1 1n 1n 1n 10n 20n)"},
                    },
                ],
                nets=[
                    {"name": "out", "pins": ["R1.1", "C1.1", "V1.+"]},
                    {"name": "0", "pins": ["R1.2", "C1.2", "V1.-"]},
                ],
            )
            assert len(circuit["components"]) == 3, circuit
            assert len(circuit["nets"]) == 2, circuit
            agent_validation = mcp.tool("schematic_validate", schematic="agent_rc")
            assert agent_validation["ok"] is True, agent_validation

            imported = mcp.tool(
                "schematic_import_spice",
                name="agent_import_rc",
                mode="drawn",
                source="* RC\nV1 in 0 PULSE(0 1 1n 1n 1n 10n 20n)\nR1 in out 1k\nC1 out 0 1n\n.end\n",
            )
            assert imported["mode"] == "drawn", imported
            dry_delete = mcp.tool("schematic_delete", schematic="agent_import_rc", dry_run=True)
            assert dry_delete["dry_run"] is True and dry_delete["blocked"] is False, dry_delete

            mcp.tool(
                "schematic_create_systemverilog",
                name="bus_buffer",
                text=(
                    "module bus_buffer(input logic [1:0] in, output logic [1:0] out);\n"
                    "    assign out = in;\n"
                    "endmodule\n"
                ),
            )
            mcp.tool("schematic_create", name="bus_top")
            bus_instance = mcp.tool(
                "schematic_add_hdl_instance",
                cell="bus_top",
                source_cell="bus_buffer",
                name="U1",
                x=0,
                y=0,
                bus_mode="flatten",
            )
            bus_pins = {pin["name"] for pin in bus_instance["pins"]}
            assert {"in[0]", "in[1]", "out[0]", "out[1]"}.issubset(bus_pins), bus_instance
            bus_ports = mcp.tool("schematic_hdl_ports", schematic="bus_buffer")
            assert bus_ports["ports"][0]["width"] == 2, bus_ports

            mixed_source = (
                "module mixed_buffer(input logic in, output logic out);\n"
                "    assign out = in;\n"
                "endmodule\n"
            )
            mcp.tool(
                "schematic_create_systemverilog",
                name="mixed_buffer",
                text=mixed_source,
            )
            mcp.tool("schematic_create", name="auto_symbol_top")
            auto_symbol = mcp.tool(
                "schematic_add_symbol",
                cell="auto_symbol_top",
                name="auto_buffer_symbol",
                source_cell="mixed_buffer",
            )
            assert {pin["name"] for pin in auto_symbol["pins"]} == {"in", "out"}, auto_symbol
            assert all(pin["width"] == 1 for pin in auto_symbol["pins"]), auto_symbol
            mcp.tool("schematic_create", name="mixed_top")
            mcp.tool(
                "schematic_add_hdl_instance",
                cell="mixed_top",
                source_cell="mixed_buffer",
                name="U1",
                x=0,
                y=0,
            )
            mcp.tool(
                "schematic_add_symbol",
                cell="mixed_top",
                name="Voltage Source",
                pins=[
                    {"name": "+", "x": -100, "y": 0, "direction": "right"},
                    {"name": "-", "x": 100, "y": 0, "direction": "right"},
                ],
            )
            mcp.tool(
                "schematic_add_instance",
                cell="mixed_top",
                symbol="Voltage Source",
                name="V1",
                x=0,
                y=0,
            )
            mcp.tool(
                "schematic_set_parameter",
                cell="mixed_top",
                instance="V1",
                name="value",
                value="PULSE(0 1 5n 1n 1n 20n 40n)",
            )
            mcp.tool(
                "schematic_add_symbol",
                cell="mixed_top",
                name="Resistor",
                pins=[
                    {"name": "1", "x": -100, "y": 0, "direction": "right"},
                    {"name": "2", "x": 100, "y": 0, "direction": "right"},
                ],
            )
            mcp.tool(
                "schematic_add_instance",
                cell="mixed_top",
                symbol="Resistor",
                name="R1",
                x=200,
                y=0,
            )
            mcp.tool(
                "schematic_set_parameter",
                cell="mixed_top",
                instance="R1",
                name="resistance",
                value="1k",
            )
            mcp.tool(
                "schematic_add_symbol",
                cell="mixed_top",
                name="Ground",
                pins=[{"name": "G", "x": 0, "y": 0, "direction": "right"}],
            )
            mcp.tool(
                "schematic_add_instance",
                cell="mixed_top",
                symbol="Ground",
                name="GND1",
                x=0,
                y=200,
            )
            mcp.tool(
                "schematic_add_net",
                cell="mixed_top",
                name="in",
                pins=["U1.in", "V1.+"],
            )
            mcp.tool(
                "schematic_add_net",
                cell="mixed_top",
                name="0",
                pins=["V1.-", "R1.2", "GND1.G"],
            )
            mcp.tool(
                "schematic_add_net",
                cell="mixed_top",
                name="out",
                pins=["U1.out", "R1.1"],
            )
            hdl_ports = mcp.tool("schematic_hdl_ports", schematic="mixed_buffer")
            assert hdl_ports["module"] == "mixed_buffer", hdl_ports
            assert {port["name"] for port in hdl_ports["ports"]} == {"in", "out"}, hdl_ports
            mixed_validation = mcp.tool("schematic_validate_hierarchy", schematic="mixed_top")
            assert mixed_validation["ok"] is True, mixed_validation
            assert mixed_validation["hdl_instances"][0]["instance"] == "U1", mixed_validation

            mcp.tool(
                "simulation_create",
                name="mixed_sim",
                schematic="mixed_top",
                stop=20e-9,
                step=1e-9,
                signals="v(out)",
            )
            mixed_run = mcp.tool("simulation_run", simulation="mixed_sim")
            assert mixed_run["state"] == "succeeded", mixed_run
            assert mixed_run["analysis"] == "mixed", mixed_run
            mixed_signals = set(mixed_run["available_signals"])
            assert any(name == "U1.out" or name.endswith(".U1.out") for name in mixed_signals), mixed_run
            assert any(name.lower() in {"v(out)", "out"} for name in mixed_signals), mixed_run
            sampled = mcp.tool(
                "simulation_trace", simulation="mixed_sim", signals=["U1.out"], max_points=16
            )
            assert sampled["returned_points"] <= 16, sampled
            assert sampled["signals"][0]["name"].endswith("U1.out"), sampled
            mcp.tool(
                "simulation_create",
                name="mixed_bad_probe",
                schematic="mixed_top",
                stop=20e-9,
                step=1e-9,
                signals="v(U1.out)",
            )
            bad_probe = mcp.tool("simulation_run", simulation="mixed_bad_probe")
            assert bad_probe["state"] == "succeeded", bad_probe
            assert any("without v(" in hint for hint in bad_probe.get("hints", [])), bad_probe

            mcp.tool(
                "simulation_create",
                name="rc_sim",
                schematic="text_test",
                analysis="tran",
                stop=1e-6,
                step=1e-9,
                signals="in,out",
            )
            run = mcp.tool("simulation_run", simulation="rc_sim")
            assert run["state"] == "succeeded", run
            assert run["trace_count"] >= 2, run
            assert "available_signals" in run and "netlist" in run
            stored = mcp.tool("simulation_result", simulation="rc_sim")
            assert stored["state"] == "succeeded", stored
            signals = mcp.tool("simulation_signals", simulation="rc_sim")
            assert {"in", "out"}.issubset(set(signals["available_signals"]))
            measured = mcp.tool("simulation_measure", simulation="rc_sim", signal="out")
            assert measured["measurements"], measured
            assert "zero_crossings" in measured["measurements"][0], measured
            assert "amplitude" in measured["measurements"][0], measured

            pdk = mcp.tool("pdk_info")
            assert "pdks" in pdk, pdk

            migrated = mcp.tool("project_migrate")
            assert migrated.get("guidance_version") == 3, migrated

            doctor = mcp.tool("ice_doctor")
            assert "pdk_roots" in doctor
            assert "pdks" in doctor
            assert "simulation_model_libraries" in doctor
            assert "timing_libraries" in doctor
            assert "model_libraries" in doctor
            assert "ngspice_executable_available" in doctor
            assert "build" in doctor and doctor["build"].get("commit"), doctor

            # Recovery: a malformed sidecar file is quarantined instead of
            # preventing the project from opening.
            (project / "sch" / "broken.icesch").write_text(
                '{"type":"text","pdk":"generic","text":"* broken"}'
            )
            reopened = mcp.tool("project_open", path=str(project), recover=True)
            assert reopened.get("quarantined_files"), reopened
            assert not (project / "sch" / "broken.icesch").exists()

            (project / "sch" / "broken2.icesch").write_text(
                '{"type":"text","pdk":"generic","text":"* broken again"}'
            )
            repaired = mcp.tool("project_repair")
            assert repaired.get("repaired") is True, repaired
            assert repaired.get("quarantined_files"), repaired

            # Project-aware model validation reports missing include files.
            mcp.tool(
                "schematic_create_text",
                name="bad_model",
                text='* bad model\n.include "/tmp/ice-does-not-exist-model.lib"\n'
                "V1 in 0 1\nR1 in out 1k\nC1 out 0 1p\n.end\n",
            )
            mcp.tool("simulation_create", name="bad_sim", schematic="bad_model")
            bad_doctor = mcp.tool("ice_doctor", simulation="bad_sim")
            assert bad_doctor["model_paths_ok"] is False, bad_doctor
            assert any(not check["exists"] for check in bad_doctor["model_checks"])

            # A server started without --project can discover and open the
            # project in its working directory.
            auto_mcp = MCP(cwd=project)
            try:
                status = auto_mcp.tool("project_summary")
                assert status["open"] is False, status
                assert status["candidates"], status
                opened = auto_mcp.tool("project_open")
                assert opened["open"] is True, opened
            finally:
                auto_mcp.close()

            print("ice-mcp tool smoke tests passed")
            return 0
        finally:
            mcp.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"ice-mcp tool smoke tests failed: {error}", file=sys.stderr)
        sys.exit(1)
