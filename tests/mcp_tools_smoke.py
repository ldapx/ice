#!/usr/bin/env python3
"""Smoke-test the ice-mcp schematic, simulation, and diagnostics tools.

Run after building ice-mcp and ice-cli:
    python3 tests/mcp_tools_smoke.py
"""

import json
import os
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

    def tool_error(self, tool_name: str, **arguments) -> str:
        """Call a tool that is expected to fail and return its error text."""
        result = self.call("tools/call", {"name": tool_name, "arguments": arguments})
        if not result.get("isError"):
            raise RuntimeError(f"{tool_name} unexpectedly succeeded")
        return result["content"][0]["text"]

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
                "schematic_wrap",
                "project_repair",
                "simulation_result",
                "simulation_signals",
                "simulation_measure",
                "simulation_corner_defaults",
                "simulation_experiment",
                "pdk_info",
                "pdk_build",
                "schematic_create_systemverilog",
                "schematic_hdl_ports",
                "schematic_add_hdl_instance",
                "schematic_set_parameter",
                "schematic_validate_hierarchy",
                "schematic_add_component",
                "schematic_list_symbols",
                "schematic_add_circuit",
                "schematic_validate",
                "schematic_set_cell_parameter",
                "schematic_remove_cell_parameter",
                "schematic_import_spice",
                "simulation_trace",
                "simulation_runs",
                "simulation_waveform",
                "ice_help",
                "layout_add_instance",
                "layout_add_port",
                "layout_duplicate",
                "layout_array",
                "layout_arrange",
                "layout_bind_device",
                "layout_unbind_device",
                "layout_device_links",
                "layout_link_schematic",
                "layout_unlink_schematic",
                "layout_create_procedural",
                "layout_regenerate",
                "layout_get_script",
                "layout_set_script",
                "layout_set_parameter",
                "layout_list_ports",
                "layout_update_port",
                "layout_remove_port",
                "layout_add_net",
                "layout_assign_port_net",
                "layout_info",
                "layout_connectivity",
                "layout_list",
                "layout_import_gds",
                "layout_import_lef",
                "layout_export_gds",
                "layout_export_lef",
                "layout_export_def",
                "layout_import_def",
                "pex_run",
                "implementation_create",
                "implementation_update",
                "implementation_list",
                "implementation_status",
                "implementation_toolchain",
                "implementation_prepare",
                "implementation_map",
                "implementation_run",
                "implementation_log",
                "implementation_cancel",
                "implementation_import_layout",
                "implementation_export_gds",
                "implementation_delete",
                "drc_run",
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
            assert "implementation_gds" in version.get("features", []), version
            assert "drc_gdscheck" in version.get("features", []), version
            assert "layout_references" in version.get("features", []), version
            assert "project_migrate" not in version.get("features", []), version

            # Keep the smoke test independent of optional PDK packages.
            mcp.tool("schematic_create", name="ring_test", pdk="generic")
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
            wrapped = mcp.tool(
                "schematic_wrap",
                schematic="ring_test",
                instances="I1,I2",
                name="ring_wrapped",
            )
            assert wrapped["cell"], wrapped
            assert wrapped["instance"], wrapped
            wrapped_info = mcp.tool("schematic_info", schematic="ring_wrapped")
            assert len(wrapped_info["instances"]) == 2, wrapped_info
            assert len(wrapped_info["nets"]) == 1, wrapped_info

            mcp.tool("layout_create", name="pin_test")
            mcp.tool("layout_create", name="arrange_test")
            mcp.tool("layout_add_layer", name="Metal1", number=1)
            arrange_rect = mcp.tool(
                "layout_add_rectangle",
                cell="arrange_test",
                layer="Metal1",
                x=0,
                y=0,
                width=100,
                height=100,
            )
            arrange_rect2 = mcp.tool(
                "layout_add_rectangle",
                cell="arrange_test",
                layer="Metal1",
                x=300,
                y=50,
                width=50,
                height=50,
            )
            duplicate = mcp.tool(
                "layout_duplicate",
                cell="arrange_test",
                shapes=str(arrange_rect["id"]),
            )
            assert len(duplicate["shapes"]) == 1, duplicate
            array = mcp.tool(
                "layout_array",
                cell="arrange_test",
                shapes=str(arrange_rect["id"]),
                rows=2,
                columns=2,
                x_pitch=200,
                y_pitch=150,
            )
            assert len(array["shapes"]) == 3, array
            mcp.tool(
                "layout_arrange",
                cell="arrange_test",
                shapes=f"{arrange_rect['id']},{arrange_rect2['id']}",
                operation="left",
            )

            mcp.tool("schematic_create", name="bind_test")
            bind_component = mcp.tool(
                "schematic_add_component",
                cell="bind_test",
                type="resistor",
                name="R1",
                x=0,
                y=0,
            )
            bind_layout = mcp.tool(
                "layout_create_from_schematic",
                schematic="bind_test",
                name="bind_layout",
            )
            bind_links = mcp.tool("layout_device_links", cell="bind_layout")
            assert len(bind_links["links"]) == 1, bind_links
            first_link = bind_links["links"][0]
            mcp.tool(
                "layout_unbind_device",
                cell="bind_layout",
                device=str(first_link["layout_object"]),
            )
            assert not mcp.tool("layout_device_links", cell="bind_layout")["links"]
            mcp.tool(
                "layout_bind_device",
                cell="bind_layout",
                device=str(first_link["layout_object"]),
                schematic="bind_test",
                schematic_device=str(bind_component["instance"]),
            )
            assert len(mcp.tool("layout_device_links", cell="bind_layout")["links"]) == 1
            bind_report = mcp.tool(
                "lvs_run",
                layout="bind_layout",
                schematic="bind_test",
            )
            assert bind_report["schema_version"] == 2, bind_report
            assert bind_report["devices"], bind_report
            assert bind_report["devices"][0]["method"] == "linked", bind_report
            bind_layout_info = mcp.tool("layout_info", cell="bind_layout")
            assert bind_layout_info["schematic"] == "bind_test", bind_layout_info
            mcp.tool("layout_unlink_schematic", cell="bind_layout")
            assert mcp.tool("layout_info", cell="bind_layout")["schematic"] is None
            mcp.tool("layout_link_schematic", cell="bind_layout", schematic="bind_test")
            assert mcp.tool("layout_info", cell="bind_layout")["schematic"] == "bind_test"

            procedural = mcp.tool("layout_create_procedural", name="proc_test")
            assert procedural["generated"], procedural
            assert procedural["generated_cell"], procedural
            script_info = mcp.tool("layout_get_script", layout="proc_test")
            assert script_info["parameters"], script_info
            parameter_name = script_info["parameters"][0]["name"]
            parameter = mcp.tool(
                "layout_set_parameter",
                layout="proc_test",
                name=parameter_name,
                value="6",
            )
            assert parameter["needs_regeneration"] is True, parameter
            mcp.tool("layout_regenerate", layout="proc_test")
            script_info = mcp.tool("layout_get_script", layout="proc_test")
            assert script_info["needs_regeneration"] is False, script_info
            script = script_info["script"]
            assert "layout.layer" in script, script
            mcp.tool(
                "layout_set_script",
                layout="proc_test",
                script=script + "\nmetal1.rectangle(0, 0, 100, 100)\n",
            )
            procedural_info = mcp.tool("layout_info", cell="proc_test")
            assert procedural_info["procedural"] is True, procedural_info
            assert procedural_info["needs_regeneration"] is True, procedural_info
            regenerated = mcp.tool("layout_regenerate", layout="proc_test")
            assert regenerated["ok"] is True, regenerated
            procedural_info = mcp.tool("layout_info", cell="proc_test")
            assert procedural_info["needs_regeneration"] is False, procedural_info
            route_script = (
                'metal1 = layout.layer("Metal1", 1)\n'
                'metal1.rectangle(0, 0, 100, 100, net="A")\n'
                'metal1.rectangle(1000, 0, 100, 100, net="A")\n'
                "layout.route((0, 0, 100, 100), (1000, 0, 100, 100),\n"
                '             "Metal1", 50, net="A",\n'
                '             start_side="right", end_side="left")\n'
            )
            mcp.tool("layout_set_script", layout="proc_test", script=route_script)
            regenerated = mcp.tool("layout_regenerate", layout="proc_test")
            assert regenerated["ok"] is True, regenerated
            connectivity = mcp.tool(
                "layout_connectivity", cell=str(procedural["generated_cell"])
            )
            assert any(
                net["name"] == "A" and len(net["shapes"]) >= 3
                for net in connectivity["nets"]
            ), connectivity
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
            layouts = mcp.tool("layout_list", limit=1)
            assert len(layouts["layouts"]) == 1, layouts
            assert layouts["pagination"]["total"] >= 2, layouts
            dry_delete = mcp.tool("layout_delete", layout="pin_test", dry_run=True)
            assert dry_delete["deleted"] is False and dry_delete["dry_run"] is True, (
                dry_delete
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
            imported_gds = mcp.tool(
                "layout_import_gds",
                path=str(exports / "pin_test.gds"),
                name_prefix="gds",
            )
            assert imported_gds["cells"], imported_gds
            imported_lef = mcp.tool(
                "layout_import_lef",
                path=str(exports / "pin_test.lef"),
                name_prefix="lef",
            )
            assert imported_lef["cells"], imported_lef
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
            dry_sim = mcp.tool("simulation_delete", simulation="sv_sim", dry_run=True)
            assert dry_sim["deleted"] is False and dry_sim["dry_run"] is True, dry_sim
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
            for topic in ("digital", "implementation", "verification"):
                topic_help = mcp.tool("ice_help", topic=topic)
                assert "help" in topic_help, topic_help
            toolchain = mcp.tool("implementation_toolchain")
            assert (
                "openroad" in toolchain
                and "yosys" in toolchain
                and "klayout" in toolchain
            ), toolchain
            symbols = mcp.tool("schematic_list_symbols", query="resistor")
            assert any(symbol["name"] == "Resistor" for symbol in symbols["symbols"]), (
                symbols
            )

            mcp.tool("schematic_create", name="alias_symbols")
            alias_symbol = mcp.tool(
                "schematic_add_symbol", cell="alias_symbols", name="RES"
            )
            assert alias_symbol["artwork"] is True, alias_symbol
            assert {pin["name"] for pin in alias_symbol["pins"]} == {"1", "2"}, (
                alias_symbol
            )
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
                    {
                        "type": "resistor",
                        "name": "R1",
                        "parameters": {"resistance": "10k"},
                    },
                    {
                        "type": "capacitor",
                        "name": "C1",
                        "parameters": {"capacitance": "1n"},
                    },
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
            dry_delete = mcp.tool(
                "schematic_delete", schematic="agent_import_rc", dry_run=True
            )
            assert dry_delete["dry_run"] is True and dry_delete["blocked"] is False, (
                dry_delete
            )

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
            assert {"in[0]", "in[1]", "out[0]", "out[1]"}.issubset(bus_pins), (
                bus_instance
            )
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
            assert {pin["name"] for pin in auto_symbol["pins"]} == {"in", "out"}, (
                auto_symbol
            )
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
            assert {port["name"] for port in hdl_ports["ports"]} == {"in", "out"}, (
                hdl_ports
            )
            mixed_validation = mcp.tool(
                "schematic_validate_hierarchy", schematic="mixed_top"
            )
            assert mixed_validation["ok"] is True, mixed_validation
            assert mixed_validation["hdl_instances"][0]["instance"] == "U1", (
                mixed_validation
            )

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
            assert any(
                name == "U1.out" or name.endswith(".U1.out") for name in mixed_signals
            ), mixed_run
            assert any(name.lower() in {"v(out)", "out"} for name in mixed_signals), (
                mixed_run
            )
            sampled = mcp.tool(
                "simulation_trace",
                simulation="mixed_sim",
                signals=["U1.out"],
                max_points=16,
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
            assert any("without v(" in hint for hint in bad_probe.get("hints", [])), (
                bad_probe
            )

            impl = mcp.tool(
                "implementation_create",
                name="sv_impl",
                top="sv_kind",
                pdk="sky130",
                source_cells=["sv_kind"],
                utilization=0.4,
                threads=2,
            )
            assert impl["top"] == "sv_kind", impl
            updated_impl = mcp.tool(
                "implementation_update",
                run="sv_impl",
                **{"from": "floorplan"},
                utilization=0.45,
                cts_enabled=False,
                timing_driven=True,
            )
            assert updated_impl["status"] == "pending", updated_impl
            deleted_impl = mcp.tool("implementation_delete", run="sv_impl")
            assert deleted_impl["deleted"] is True, deleted_impl

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
            slew = mcp.tool(
                "simulation_measure",
                simulation="rc_sim",
                metric="slew_rate",
                signal="out",
            )
            assert slew["valid"] is True, slew
            configured = mcp.tool(
                "simulation_update",
                simulation="rc_sim",
                temperature=27,
                corners=[
                    {"name": "tt", "temperature": 27},
                    {"name": "ff", "temperature": 85, "source_overrides": {"V1": 1.1}},
                ],
                measurements=[
                    {"name": "slew", "metric": "slew_rate", "signal": "out"},
                ],
            )
            assert len(configured["corners"]) == 2, configured
            experiment = mcp.tool(
                "simulation_experiment",
                simulation="rc_sim",
                timeout_ms=30000,
                store=False,
            )
            assert len(experiment["points"]) == 2, experiment
            assert experiment["points"][0]["state"] == "succeeded", experiment
            assert experiment["points"][0]["measurements"], experiment

            graphs = mcp.tool("simulation_graph_list", simulation="rc_sim")
            assert graphs, graphs
            graph_id = graphs[0]["id"]
            analyzed = mcp.tool(
                "simulation_graph_update",
                simulation="rc_sim",
                graph=graph_id,
                view="db",
                fft={"enabled": True, "window": "hann", "points": 512},
                derived_traces=[{"name": "slope", "expression": "derivative(out)"}],
                markers=[{"label": "M1", "signal": "out", "x": 1e-7, "y": 0.1}],
            )
            assert analyzed["view"] == "db", analyzed
            assert analyzed["fft"]["enabled"] is True, analyzed
            assert analyzed["derived_traces"][0]["name"] == "slope", analyzed
            assert analyzed["markers"][0]["label"] == "M1", analyzed

            runs = mcp.tool("simulation_runs", simulation="rc_sim")
            assert runs["runs"], runs
            overlaid = mcp.tool(
                "simulation_graph_update",
                simulation="rc_sim",
                graph=graph_id,
                overlays=[{"run_id": runs["runs"][0]["run_id"], "label": "tt"}],
            )
            assert overlaid["overlays"], overlaid

            waveform = mcp.tool(
                "simulation_waveform",
                simulation="rc_sim",
                signals="out",
                view="db",
                derived=[{"name": "slope", "expression": "derivative(out)"}],
                max_points=64,
            )
            names = [series["name"] for series in waveform["series"]]
            assert "slope" in names, waveform
            assert all(len(series["x"]) <= 65 for series in waveform["series"]), (
                waveform
            )
            spectrum = mcp.tool(
                "simulation_waveform",
                simulation="rc_sim",
                signals="out",
                fft={"enabled": True, "window": "hann", "points": 512},
            )
            assert spectrum["series"][0]["x_unit"] == "Hz", spectrum

            pdk = mcp.tool("pdk_info")
            assert "pdks" in pdk, pdk

            doctor = mcp.tool("ice_doctor")
            assert "pdk_roots" in doctor
            assert "pdks" in doctor
            assert "simulation_model_libraries" in doctor
            assert "timing_libraries" in doctor
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

            # A drawn cell netlist must expose its subcircuit port order.
            mcp.tool("schematic_create", name="ports_test")
            mcp.tool(
                "schematic_add_component",
                cell="ports_test",
                symbol="Input",
                name="A",
                x=0,
                y=0,
            )
            mcp.tool(
                "schematic_add_component",
                cell="ports_test",
                symbol="Output",
                name="Y",
                x=200,
                y=0,
            )
            netlist = mcp.tool("schematic_netlist", schematic="ports_test")
            assert netlist["ports"] == ["A", "Y"], netlist
            parameter = mcp.tool(
                "schematic_set_cell_parameter",
                cell="ports_test",
                name="R",
                value="1k",
            )
            assert parameter["value"] == "1k", parameter
            info = mcp.tool("schematic_info", cell="ports_test")
            assert any(entry["name"] == "R" for entry in info["parameters"]), info
            paged_info = mcp.tool("schematic_info", cell="ports_test", limit=1)
            assert len(paged_info["parameters"]) <= 1, paged_info
            assert paged_info["pagination"]["parameters"]["total"] >= 1, paged_info
            removed = mcp.tool(
                "schematic_remove_cell_parameter", cell="ports_test", name="R"
            )
            assert removed["removed"] is True, removed

            mcp.tool("schematic_create", name="pin_test")
            pin = mcp.tool(
                "schematic_add_component",
                cell="pin_test",
                type="pin",
                name="P1",
                x=0,
                y=0,
            )
            assert pin["pins"], pin
            pin_netlist = mcp.tool("schematic_netlist", schematic="pin_test")
            assert pin_netlist["ports"] == ["P1"], pin_netlist

            # A mutation whose automatic save fails must be reported and rolled
            # back instead of leaving the server unable to save anything.
            locked = [
                project,
                project / "sch",
                project / "lay",
                project / "sim",
                project / "impl",
            ]
            try:
                for path in locked:
                    if path.is_dir():
                        os.chmod(path, 0o555)
                error = mcp.tool_error(
                    "schematic_add_component",
                    cell="ring_test",
                    symbol="Output",
                    name="LOCKED",
                )
                assert "discarded" in error, error
                summary = mcp.tool("project_summary")
                assert summary.get("schematics") is not None, summary
            finally:
                for path in locked:
                    if path.is_dir():
                        os.chmod(path, 0o755)
            reloaded = mcp.tool("project_reload", discard=True)
            assert reloaded.get("schematics") is not None, reloaded

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

            paged_summary = mcp.tool("project_summary", limit=1)
            assert len(paged_summary["schematics"]) <= 1, paged_summary
            assert paged_summary["pagination"]["schematics"]["total"] >= 1, (
                paged_summary
            )
            chunk = mcp.call(
                "resources/read", {"uri": "ice://project/json?offset=0&limit=64"}
            )
            chunk_data = json.loads(chunk["contents"][0]["text"])
            assert chunk_data["total_bytes"] > 0, chunk_data
            assert len(chunk_data["data"]) <= 64, chunk_data

            with tempfile.TemporaryDirectory(prefix="ice-mcp-overwrite-") as tmp:
                overwrite_root = Path(tmp) / "guard"
                guard = MCP(cwd=Path(tmp))
                try:
                    guard.tool("project_create", path=str(overwrite_root), name="first")
                    guard.tool("schematic_create", name="keep", kind="drawn")
                    error = guard.tool_error(
                        "project_create", path=str(overwrite_root), name="second"
                    )
                    assert "already exists" in error, error
                    overwritten = guard.tool(
                        "project_create",
                        path=str(overwrite_root),
                        name="second",
                        overwrite=True,
                    )
                    assert overwritten["name"] == "second", overwritten
                    assert overwritten["schematics"] == [], overwritten
                finally:
                    guard.close()

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
